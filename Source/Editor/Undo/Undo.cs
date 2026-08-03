// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Linq;
using FlaxEditor.History;
using FlaxEditor.Utilities;
using FlaxEngine.Collections;

namespace FlaxEditor
{
    /// <summary>
    /// The undo/redo actions recording object.
    /// </summary>
    public class Undo : IDisposable
    {
        /// <summary>
        /// Undo action wrapper used to publish local undo context actions into a parent undo timeline.
        /// </summary>
        public sealed class LinkedUndoAction : IUndoAction, IUndoActionMetadata
        {
            private Undo _undo;
            private IUndoAction _action;

            /// <summary>
            /// Gets the source undo context.
            /// </summary>
            public Undo SourceUndo => _undo;

            /// <summary>
            /// Gets the owner object of the source undo context.
            /// </summary>
            public object Owner { get; }

            /// <summary>
            /// Initializes a new instance of the <see cref="LinkedUndoAction"/> class.
            /// </summary>
            /// <param name="undo">The source undo context.</param>
            /// <param name="action">The source undo action.</param>
            /// <param name="owner">The source undo owner.</param>
            public LinkedUndoAction(Undo undo, IUndoAction action, object owner)
            {
                _undo = undo ?? throw new ArgumentNullException(nameof(undo));
                _action = action ?? throw new ArgumentNullException(nameof(action));
                Owner = owner;
            }

            /// <inheritdoc />
            public string ActionString => _action?.ActionString;

            /// <inheritdoc />
            public UndoActionInfo ActionInfo
            {
                get
                {
                    var info = UndoActionMetadata.GetActionInfo(_action).Clone();
                    if (_action != null && string.IsNullOrEmpty(info.Operation))
                        info.Operation = _action.ActionString;
                    if (Owner != null && string.IsNullOrEmpty(info.DisplayEditorTypeName))
                        info.DisplayEditorTypeName = Owner.GetType().FullName;
                    return info;
                }
            }

            internal bool References(Undo undo, IUndoAction action)
            {
                return ReferenceEquals(_undo, undo) && ReferenceEquals(_action, action);
            }

            /// <inheritdoc />
            public void Do()
            {
                _undo?.PerformLinkedRedo(_action);
            }

            /// <inheritdoc />
            public void Undo()
            {
                _undo?.PerformLinkedUndo(_action);
            }

            /// <inheritdoc />
            public void Dispose()
            {
                var undo = _undo;
                var action = _action;
                _undo = null;
                _action = null;
                undo?.RemoveLinkedAction(action);
            }
        }

        /// <summary>
        /// Undo system event.
        /// </summary>
        /// <param name="action">The action.</param>
        public delegate void UndoEventDelegate(IUndoAction action);

        internal interface IUndoInternal
        {
            /// <summary>
            /// Creates the undo action object on recording end.
            /// </summary>
            /// <param name="snapshotInstance">The snapshot object.</param>
            /// <returns>The undo action. May be null if no changes found.</returns>
            IUndoAction End(object snapshotInstance);
        }

        /// <summary>
        /// Stack of undo actions for future disposal.
        /// </summary>
        private readonly OrderedDictionary<object, IUndoInternal> _snapshots = new OrderedDictionary<object, IUndoInternal>();
        private readonly Undo _parentUndo;
        private readonly object _parentOwner;
        private bool _isSyncingLinkedHistory;
        private int _performingUndoRedoDepth;

        /// <summary>
        /// Gets the undo operations stack.
        /// </summary>
        public HistoryStack UndoOperationsStack { get; }

        /// <summary>
        /// Occurs when undo operation is done.
        /// </summary>
        public event UndoEventDelegate UndoDone;

        /// <summary>
        /// Occurs when redo operation is done.
        /// </summary>
        public event UndoEventDelegate RedoDone;

        /// <summary>
        /// Occurs when action is done and appended to the <see cref="Undo"/>.
        /// </summary>
        public event UndoEventDelegate ActionDone;

        /// <summary>
        /// Occurs when an action is discarded from history and reports why it was removed.
        /// </summary>
        public event Action<IUndoAction, HistoryStackDiscardReason> ActionDiscarded;

        /// <summary>
        /// Gets or sets a value indicating whether this <see cref="Undo"/> is enabled.
        /// </summary>
        public virtual bool Enabled { get; set; } = true;

        /// <summary>
        /// Gets a value indicating whether can do undo on last performed action.
        /// </summary>
        public bool CanUndo => (_parentUndo ?? this).UndoOperationsStack.HistoryCount > 0;

        /// <summary>
        /// Gets a value indicating whether can do redo on last undone action.
        /// </summary>
        public bool CanRedo => (_parentUndo ?? this).UndoOperationsStack.ReverseCount > 0;

        /// <summary>
        /// Gets a value indicating whether this undo context or its parent is currently replaying undo/redo.
        /// </summary>
        public bool IsPerformingUndoRedo => _performingUndoRedoDepth != 0 || (_parentUndo?.IsPerformingUndoRedo ?? false);

        /// <summary>
        /// Gets the first name of the undo action.
        /// </summary>
        public string FirstUndoName => (_parentUndo ?? this).UndoOperationsStack.PeekHistory().ActionString;

        /// <summary>
        /// Gets metadata for the first undo action.
        /// </summary>
        public UndoActionInfo FirstUndoInfo => UndoActionMetadata.GetActionInfo((IUndoAction)(_parentUndo ?? this).UndoOperationsStack.PeekHistory());

        /// <summary>
        /// Gets the first name of the redo action.
        /// </summary>
        public string FirstRedoName => (_parentUndo ?? this).UndoOperationsStack.PeekReverse().ActionString;

        /// <summary>
        /// Gets metadata for the first redo action.
        /// </summary>
        public UndoActionInfo FirstRedoInfo => UndoActionMetadata.GetActionInfo((IUndoAction)(_parentUndo ?? this).UndoOperationsStack.PeekReverse());

        /// <summary>
        /// Gets or sets the capacity of the undo history buffers.
        /// </summary>
        public int Capacity
        {
            get => UndoOperationsStack.HistoryActionsLimit;
            set => UndoOperationsStack.HistoryActionsLimit = value;
        }

        /// <summary>
        /// Gets or sets the approximate undo history size limit in bytes. Negative value disables size-based pruning.
        /// </summary>
        public long SizeCapacityInBytes
        {
            get => UndoOperationsStack.HistorySizeLimitInBytes;
            set => UndoOperationsStack.HistorySizeLimitInBytes = value;
        }

        /// <summary>
        /// Gets the approximate undo history size in bytes. Actions with unknown size are not counted.
        /// </summary>
        public long SizeInBytes => UndoOperationsStack.HistorySizeInBytes;

        /// <summary>
        /// Gets a snapshot of undo actions, ordered from the next undo action to the oldest undo action.
        /// </summary>
        /// <returns>The undo actions.</returns>
        public IUndoAction[] GetUndoActions()
        {
            return (_parentUndo ?? this).UndoOperationsStack.GetHistoryActions().Cast<IUndoAction>().ToArray();
        }

        /// <summary>
        /// Gets a snapshot of redo actions, ordered from the next redo action to the oldest redo action.
        /// </summary>
        /// <returns>The redo actions.</returns>
        public IUndoAction[] GetRedoActions()
        {
            return (_parentUndo ?? this).UndoOperationsStack.GetReverseActions().Cast<IUndoAction>().ToArray();
        }

        /// <summary>
        /// Gets a snapshot of undo action metadata, ordered from the next undo action to the oldest undo action.
        /// </summary>
        /// <returns>The undo action metadata.</returns>
        public UndoActionInfo[] GetUndoActionInfos()
        {
            return GetUndoActions().Select(UndoActionMetadata.GetActionInfo).ToArray();
        }

        /// <summary>
        /// Gets a snapshot of redo action metadata, ordered from the next redo action to the oldest redo action.
        /// </summary>
        /// <returns>The redo action metadata.</returns>
        public UndoActionInfo[] GetRedoActionInfos()
        {
            return GetRedoActions().Select(UndoActionMetadata.GetActionInfo).ToArray();
        }

        /// <summary>
        /// Internal class for keeping reference of undo action.
        /// </summary>
        internal class UndoInternal : IUndoInternal
        {
            public string ActionString;
            public object SnapshotInstance;
            public ObjectSnapshot Snapshot;

            public UndoInternal(object snapshotInstance, string actionString)
            {
                ActionString = actionString;
                SnapshotInstance = snapshotInstance;
                Snapshot = ObjectSnapshot.CaptureSnapshot(snapshotInstance);
            }

            /// <inheritdoc />
            public IUndoAction End(object snapshotInstance)
            {
                var diff = Snapshot.Compare(snapshotInstance);
                if (diff.Count == 0)
                    return null;
                return new UndoActionObject(diff, ActionString, SnapshotInstance);
            }
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="Undo"/> class.
        /// </summary>
        /// <param name="historyActionsLimit">The history actions limit.</param>
        public Undo(int historyActionsLimit = 1000)
        {
            UndoOperationsStack = new HistoryStack(historyActionsLimit, GetActionSizeInBytes);
            UndoOperationsStack.ActionDisposed += OnHistoryActionDisposed;
            UndoOperationsStack.ActionDiscarded += OnHistoryActionDiscarded;
        }

        /// <summary>
        /// Initializes a new instance of the <see cref="Undo"/> class linked to a parent undo timeline.
        /// </summary>
        /// <param name="parentUndo">The parent undo timeline.</param>
        /// <param name="parentOwner">The owner object of this undo context.</param>
        /// <param name="historyActionsLimit">The history actions limit.</param>
        public Undo(Undo parentUndo, object parentOwner, int historyActionsLimit = 1000)
        : this(historyActionsLimit)
        {
            if (parentUndo == null)
                throw new ArgumentNullException(nameof(parentUndo));
            if (ReferenceEquals(parentUndo, this))
                throw new ArgumentException("Cannot link undo context to itself.", nameof(parentUndo));

            _parentUndo = parentUndo;
            _parentOwner = parentOwner;
        }

        /// <summary>
        /// Begins recording for undo action.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to record.</param>
        /// <param name="actionString">Name of action to be displayed in undo stack.</param>
        public void RecordBegin(object snapshotInstance, string actionString)
        {
            if (!Enabled)
                return;

            _snapshots.Add(snapshotInstance, new UndoInternal(snapshotInstance, actionString));

        }

        /// <summary>
        /// Ends recording for undo action.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to finish recording, if null take last provided.</param>
        /// <param name="customActionBefore">Custom action to append to the undo block action before recorded modifications apply.</param>
        /// <param name="customActionAfter">Custom action to append to the undo block action after recorded modifications apply.</param>
        public void RecordEnd(object snapshotInstance = null, IUndoAction customActionBefore = null, IUndoAction customActionAfter = null)
        {
            if (!Enabled)
                return;

            if (snapshotInstance == null)
                snapshotInstance = _snapshots.Last().Key;
            var action = _snapshots[snapshotInstance].End(snapshotInstance);
            _snapshots.Remove(snapshotInstance);

            // It may be null if no changes has been found during recording
            if (action != null)
            {
                // Batch with a custom action if provided
                if (customActionBefore != null && customActionAfter != null)
                {
                    action = new MultiUndoAction(new[] { customActionBefore, action, customActionAfter });
                }
                else if (customActionBefore != null)
                {
                    action = new MultiUndoAction(new[] { customActionBefore, action });
                }
                else if (customActionAfter != null)
                {
                    action = new MultiUndoAction(new[] { action, customActionAfter });
                }

                UndoOperationsStack.Push(action);
                OnAction(action);
                AddLinkedAction(action);
            }
        }

        /// <summary>
        /// Internal class for keeping reference of undo action that modifies collection of objects.
        /// </summary>
        internal class UndoMultiInternal : IUndoInternal
        {
            public string ActionString;
            public object[] SnapshotInstances;
            public ObjectSnapshot[] Snapshot;

            public UndoMultiInternal(object[] snapshotInstances, string actionString)
            {
                ActionString = actionString;
                SnapshotInstances = snapshotInstances;
                Snapshot = new ObjectSnapshot[snapshotInstances.Length];
                for (var i = 0; i < snapshotInstances.Length; i++)
                {
                    Snapshot[i] = ObjectSnapshot.CaptureSnapshot(snapshotInstances[i]);
                }
            }

            /// <inheritdoc />
            public IUndoAction End(object snapshotInstance)
            {
                var snapshotInstances = (object[])snapshotInstance;
                if (snapshotInstances == null || snapshotInstances.Length != SnapshotInstances.Length)
                    throw new ArgumentException("Invalid multi undo action objects.");
                List<UndoActionObject> actions = null;
                for (int i = 0; i < snapshotInstances.Length; i++)
                {
                    var diff = Snapshot[i].Compare(snapshotInstances[i]);
                    if (diff.Count == 0)
                        continue;
                    if (actions == null)
                        actions = new List<UndoActionObject>();
                    actions.Add(new UndoActionObject(diff, ActionString, SnapshotInstances[i]));
                }
                if (actions == null)
                    return null;
                if (actions.Count == 1)
                    return actions[0];
                return new MultiUndoAction(actions);
            }
        }

        /// <summary>
        /// Begins recording for undo action.
        /// </summary>
        /// <param name="snapshotInstances">Instances of objects to record.</param>
        /// <param name="actionString">Name of action to be displayed in undo stack.</param>
        public void RecordMultiBegin(object[] snapshotInstances, string actionString)
        {
            if (!Enabled)
                return;

            _snapshots.Add(snapshotInstances, new UndoMultiInternal(snapshotInstances, actionString));
        }

        /// <summary>
        /// Ends recording for undo action.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to finish recording, if null take last provided.</param>
        /// <param name="customActionBefore">Custom action to append to the undo block action before recorded modifications apply.</param>
        /// <param name="customActionAfter">Custom action to append to the undo block action after recorded modifications apply.</param>
        public void RecordMultiEnd(object[] snapshotInstance = null, IUndoAction customActionBefore = null, IUndoAction customActionAfter = null)
        {
            if (!Enabled)
                return;

            if (snapshotInstance == null)
                snapshotInstance = (object[])_snapshots.Last().Key;
            var action = _snapshots[snapshotInstance].End(snapshotInstance);
            _snapshots.Remove(snapshotInstance);

            // It may be null if no changes has been found during recording
            if (action != null)
            {
                // Batch with a custom action if provided
                if (customActionBefore != null && customActionAfter != null)
                {
                    action = new MultiUndoAction(new[] { customActionBefore, action, customActionAfter });
                }
                else if (customActionBefore != null)
                {
                    action = new MultiUndoAction(new[] { customActionBefore, action });
                }
                else if (customActionAfter != null)
                {
                    action = new MultiUndoAction(new[] { action, customActionAfter });
                }

                UndoOperationsStack.Push(action);
                OnAction(action);
                AddLinkedAction(action);
            }
        }

        /// <summary>
        /// Creates new undo action for provided instance of object.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to record</param>
        /// <param name="actionString">Name of action to be displayed in undo stack.</param>
        /// <param name="actionsToSave">Action in after witch recording will be finished.</param>
        public void RecordAction(object snapshotInstance, string actionString, Action actionsToSave)
        {
            RecordBegin(snapshotInstance, actionString);
            actionsToSave?.Invoke();
            RecordEnd(snapshotInstance);
        }

        /// <summary>
        /// Creates new undo action for provided instance of object.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to record</param>
        /// <param name="actionString">Name of action to be displayed in undo stack.</param>
        /// <param name="actionsToSave">Action in after witch recording will be finished.</param>
        public void RecordAction<T>(T snapshotInstance, string actionString, Action<T> actionsToSave)
        where T : new()
        {
            RecordBegin(snapshotInstance, actionString);
            actionsToSave?.Invoke(snapshotInstance);
            RecordEnd(snapshotInstance);
        }

        /// <summary>
        /// Creates new undo action for provided instance of object.
        /// </summary>
        /// <param name="snapshotInstance">Instance of an object to record</param>
        /// <param name="actionString">Name of action to be displayed in undo stack.</param>
        /// <param name="actionsToSave">Action in after witch recording will be finished.</param>
        public void RecordAction(object snapshotInstance, string actionString, Action<object> actionsToSave)
        {
            RecordBegin(snapshotInstance, actionString);
            actionsToSave?.Invoke(snapshotInstance);
            RecordEnd(snapshotInstance);
        }

        /// <summary>
        /// Adds the action to the history.
        /// </summary>
        /// <param name="action">The action.</param>
        public void AddAction(IUndoAction action)
        {
            if (action == null)
                throw new ArgumentNullException();
            if (!Enabled)
                return;

            UndoOperationsStack.Push(action);
            OnAction(action);
            AddLinkedAction(action);
        }

        /// <summary>
        /// Undo last recorded action
        /// </summary>
        public void PerformUndo()
        {
            if (_parentUndo != null)
            {
                _parentUndo.PerformUndo();
                return;
            }

            if (!Enabled || !CanUndo)
                return;

            var action = (IUndoAction)UndoOperationsStack.PopHistory();
            _performingUndoRedoDepth++;
            try
            {
                action.Undo();
                OnUndo(action);
            }
            finally
            {
                _performingUndoRedoDepth--;
            }
        }

        /// <summary>
        /// Redo last undone action
        /// </summary>
        public void PerformRedo()
        {
            if (_parentUndo != null)
            {
                _parentUndo.PerformRedo();
                return;
            }

            if (!Enabled || !CanRedo)
                return;

            var action = (IUndoAction)UndoOperationsStack.PopReverse();
            _performingUndoRedoDepth++;
            try
            {
                action.Do();
                OnRedo(action);
            }
            finally
            {
                _performingUndoRedoDepth--;
            }
        }

        /// <summary>
        /// Removes all matching actions from this undo history.
        /// </summary>
        /// <param name="match">The match predicate.</param>
        public void RemoveActions(Predicate<IUndoAction> match)
        {
            if (match == null)
                throw new ArgumentNullException(nameof(match));

            UndoOperationsStack.RemoveAll(x => x is IUndoAction action && match(action));
        }

        private void AddLinkedAction(IUndoAction action)
        {
            _parentUndo?.AddAction(new LinkedUndoAction(this, action, _parentOwner));
        }

        private static long GetActionSizeInBytes(IHistoryAction action)
        {
            return action is IUndoAction undoAction ? UndoActionMetadata.GetActionInfo(undoAction).SizeInBytes : -1;
        }

        private bool PerformLinkedUndo(IUndoAction action)
        {
            if (action == null || !ReferenceEquals(UndoOperationsStack.PeekHistory(), action))
                return false;

            var historyAction = (IUndoAction)UndoOperationsStack.PopHistory();
            _performingUndoRedoDepth++;
            try
            {
                historyAction.Undo();
                OnUndo(historyAction);
            }
            finally
            {
                _performingUndoRedoDepth--;
            }
            return true;
        }

        private bool PerformLinkedRedo(IUndoAction action)
        {
            if (action == null || !ReferenceEquals(UndoOperationsStack.PeekReverse(), action))
                return false;

            var historyAction = (IUndoAction)UndoOperationsStack.PopReverse();
            _performingUndoRedoDepth++;
            try
            {
                historyAction.Do();
                OnRedo(historyAction);
            }
            finally
            {
                _performingUndoRedoDepth--;
            }
            return true;
        }

        private void OnHistoryActionDisposed(IHistoryAction action)
        {
            if (_parentUndo == null || _isSyncingLinkedHistory || !(action is IUndoAction undoAction))
                return;

            _isSyncingLinkedHistory = true;
            try
            {
                _parentUndo.RemoveActions(x => x is LinkedUndoAction linkedAction && linkedAction.References(this, undoAction));
            }
            finally
            {
                _isSyncingLinkedHistory = false;
            }
        }

        private void OnHistoryActionDiscarded(IHistoryAction action, HistoryStackDiscardReason reason)
        {
            if (action is IUndoAction undoAction)
                ActionDiscarded?.Invoke(undoAction, reason);
        }

        private void RemoveLinkedAction(IUndoAction action)
        {
            if (_isSyncingLinkedHistory || action == null)
                return;

            _isSyncingLinkedHistory = true;
            try
            {
                UndoOperationsStack.RemoveAll(x => ReferenceEquals(x, action));
            }
            finally
            {
                _isSyncingLinkedHistory = false;
            }
        }

        /// <summary>
        /// Called when <see cref="Undo"/> performs action.
        /// </summary>
        /// <param name="action">The action.</param>
        protected virtual void OnAction(IUndoAction action)
        {
            ActionDone?.Invoke(action);
        }

        /// <summary>
        /// Called when <see cref="Undo"/> performs undo action.
        /// </summary>
        /// <param name="action">The action.</param>
        protected virtual void OnUndo(IUndoAction action)
        {
            UndoDone?.Invoke(action);
        }

        /// <summary>
        /// Called when <see cref="Undo"/> performs redo action.
        /// </summary>
        /// <param name="action">The action.</param>
        protected virtual void OnRedo(IUndoAction action)
        {
            RedoDone?.Invoke(action);
        }

        /// <summary>
        /// Clears the history.
        /// </summary>
        public void Clear()
        {
            _parentUndo?.RemoveActions(x => x is LinkedUndoAction action && ReferenceEquals(action.SourceUndo, this));
            _snapshots.Clear();
            UndoOperationsStack.Clear();
        }

        /// <inheritdoc />
        public void Dispose()
        {
            UndoDone = null;
            RedoDone = null;
            ActionDone = null;
            ActionDiscarded = null;

            Clear();
        }
    }
}
