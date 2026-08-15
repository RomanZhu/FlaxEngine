// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.Threading;
using FlaxEngine;

namespace FlaxEditor.SceneEditing
{
    /// <summary>
    /// Opt-in structured Scene diagnostics. Failure terminals may be emitted even when verbose tracing is disabled.
    /// </summary>
    public static class SceneDebug
    {
        private static long _sequence;

        /// <summary>
        /// Gets or sets a value indicating whether verbose Scene transition diagnostics are enabled.
        /// </summary>
        public static bool Enabled { get; set; }

        /// <summary>
        /// Writes an opt-in structured state transition.
        /// </summary>
        public static void Log(string eventName, string details = null)
        {
            if (!Enabled)
                return;
            Write(false, eventName, details);
        }

        /// <summary>
        /// Writes a structured failure terminal regardless of verbose tracing.
        /// </summary>
        public static void Error(SceneMutationErrorCode code, string eventName, string details = null)
        {
            Write(true, eventName, $"Code={code} {details}".TrimEnd());
        }

        private static void Write(bool error, string eventName, string details)
        {
            var sequence = Interlocked.Increment(ref _sequence);
            var message = $"[SceneDebug #{sequence}] Event={eventName}";
            if (!string.IsNullOrWhiteSpace(details))
                message += " " + details;
            if (error)
                Editor.LogError(message);
            else
                Editor.Log(message);
        }
    }

    /// <summary>
    /// Scene mutation operation kind.
    /// </summary>
    public enum SceneMutationOperation
    {
        /// <summary>An unspecified mutation.</summary>
        Unknown,
        /// <summary>Paste Actors.</summary>
        Paste,
        /// <summary>Duplicate Actors.</summary>
        Duplicate,
        /// <summary>Delete Actors.</summary>
        Delete,
        /// <summary>Restore deleted Actors.</summary>
        Restore,
        /// <summary>Change Scene object ownership.</summary>
        Reparent,
        /// <summary>Group Scene objects.</summary>
        Group,
        /// <summary>Spawn Scene objects.</summary>
        Spawn,
        /// <summary>Convert an Actor type.</summary>
        Convert,
        /// <summary>Mutate a Prefab or Prefab instance.</summary>
        Prefab,
        /// <summary>Save a Scene.</summary>
        Save,
        /// <summary>Undo a mutation.</summary>
        Undo,
        /// <summary>Redo a mutation.</summary>
        Redo,
    }

    /// <summary>
    /// Stable scene mutation failure code used by automation and diagnostics.
    /// </summary>
    public enum SceneMutationErrorCode
    {
        /// <summary>No error.</summary>
        None,
        /// <summary>The serialized input is invalid.</summary>
        InvalidPayload,
        /// <summary>The mutation has no destination.</summary>
        MissingDestination,
        /// <summary>The destination Scene is not loaded.</summary>
        DestinationUnloaded,
        /// <summary>The destination cannot be changed.</summary>
        DestinationReadOnly,
        /// <summary>A required object type is unavailable.</summary>
        MissingType,
        /// <summary>A stable reference could not be resolved.</summary>
        ReferenceResolutionFailed,
        /// <summary>Object construction failed.</summary>
        ConstructionFailed,
        /// <summary>Publishing staged objects failed.</summary>
        PublicationFailed,
        /// <summary>The committed state failed verification.</summary>
        PostconditionFailed,
        /// <summary>A dependency required for replay is unavailable.</summary>
        ReplayDependencyMissing,
        /// <summary>Scene persistence failed.</summary>
        SaveFailed,
        /// <summary>Restoring the previous state failed.</summary>
        RollbackFailed,
    }

    /// <summary>
    /// Terminal scene mutation status.
    /// </summary>
    public enum SceneMutationStatus
    {
        /// <summary>The mutation committed.</summary>
        Success,
        /// <summary>Preflight rejected the mutation without changing state.</summary>
        Rejected,
        /// <summary>The mutation failed after starting and attempted rollback.</summary>
        Failed,
    }

    /// <summary>
    /// Structured result of a scene mutation.
    /// </summary>
    public sealed class SceneMutationResult
    {
        /// <summary>
        /// Gets the transaction identifier.
        /// </summary>
        public Guid TransactionId { get; }

        /// <summary>
        /// Gets the operation kind.
        /// </summary>
        public SceneMutationOperation Operation { get; }

        /// <summary>
        /// Gets the terminal status.
        /// </summary>
        public SceneMutationStatus Status { get; }

        /// <summary>
        /// Gets the stable error code.
        /// </summary>
        public SceneMutationErrorCode ErrorCode { get; }

        /// <summary>
        /// Gets the diagnostic message.
        /// </summary>
        public string Message { get; }

        /// <summary>
        /// Gets the affected scene identifiers.
        /// </summary>
        public Guid[] SceneIds { get; }

        /// <summary>
        /// Gets the created object identifiers.
        /// </summary>
        public Guid[] CreatedObjectIds { get; }

        /// <summary>
        /// Gets the removed object identifiers.
        /// </summary>
        public Guid[] RemovedObjectIds { get; }

        /// <summary>
        /// Gets the warnings produced by an otherwise successful operation.
        /// </summary>
        public string[] Warnings { get; }

        /// <summary>
        /// Gets a value indicating whether rollback completed and was verified.
        /// </summary>
        public bool RollbackCompleted { get; }

        /// <summary>
        /// Gets a value indicating whether the operation succeeded.
        /// </summary>
        public bool Succeeded => Status == SceneMutationStatus.Success;

        private SceneMutationResult(Guid transactionId, SceneMutationOperation operation, SceneMutationStatus status, SceneMutationErrorCode errorCode, string message, Guid[] sceneIds, Guid[] createdObjectIds, Guid[] removedObjectIds, string[] warnings, bool rollbackCompleted)
        {
            TransactionId = transactionId;
            Operation = operation;
            Status = status;
            ErrorCode = errorCode;
            Message = message ?? string.Empty;
            SceneIds = sceneIds ?? Array.Empty<Guid>();
            CreatedObjectIds = createdObjectIds ?? Array.Empty<Guid>();
            RemovedObjectIds = removedObjectIds ?? Array.Empty<Guid>();
            Warnings = warnings ?? Array.Empty<string>();
            RollbackCompleted = rollbackCompleted;
        }

        internal static SceneMutationResult Success(Guid transactionId, SceneMutationOperation operation, IEnumerable<Guid> sceneIds = null, IEnumerable<Guid> createdObjectIds = null, IEnumerable<Guid> removedObjectIds = null, IEnumerable<string> warnings = null)
        {
            return new SceneMutationResult(transactionId, operation, SceneMutationStatus.Success, SceneMutationErrorCode.None, string.Empty, ToArray(sceneIds), ToArray(createdObjectIds), ToArray(removedObjectIds), ToArray(warnings), false);
        }

        internal static SceneMutationResult Rejected(Guid transactionId, SceneMutationOperation operation, SceneMutationErrorCode errorCode, string message, IEnumerable<Guid> sceneIds = null)
        {
            return new SceneMutationResult(transactionId, operation, SceneMutationStatus.Rejected, errorCode, message, ToArray(sceneIds), null, null, null, false);
        }

        internal static SceneMutationResult Failed(Guid transactionId, SceneMutationOperation operation, SceneMutationErrorCode errorCode, string message, bool rollbackCompleted, IEnumerable<Guid> sceneIds = null, IEnumerable<Guid> createdObjectIds = null)
        {
            return new SceneMutationResult(transactionId, operation, SceneMutationStatus.Failed, errorCode, message, ToArray(sceneIds), ToArray(createdObjectIds), null, null, rollbackCompleted);
        }

        private static T[] ToArray<T>(IEnumerable<T> values)
        {
            if (values == null)
                return Array.Empty<T>();
            if (values is T[] array)
                return array;
            return new List<T>(values).ToArray();
        }
    }

    /// <summary>
    /// Immutable preflight description of an Actor mutation.
    /// </summary>
    public sealed class SceneMutationPlan
    {
        /// <summary>
        /// Gets the transaction identifier.
        /// </summary>
        public Guid TransactionId { get; }

        /// <summary>
        /// Gets the operation kind.
        /// </summary>
        public SceneMutationOperation Operation { get; }

        /// <summary>
        /// Gets the destination Scene identifier.
        /// </summary>
        public Guid DestinationSceneId { get; }

        /// <summary>
        /// Gets the destination parent Actor identifier.
        /// </summary>
        public Guid DestinationParentId { get; }

        /// <summary>
        /// Gets the serialized payload version.
        /// </summary>
        public int PayloadVersion { get; }

        /// <summary>
        /// Gets the source object identifiers contained in the payload.
        /// </summary>
        public Guid[] PayloadObjectIds { get; }

        internal SceneMutationPlan(SceneMutationOperation operation, Guid destinationSceneId, Guid destinationParentId, int payloadVersion, Guid[] payloadObjectIds)
        {
            TransactionId = Guid.NewGuid();
            Operation = operation;
            DestinationSceneId = destinationSceneId;
            DestinationParentId = destinationParentId;
            PayloadVersion = payloadVersion;
            PayloadObjectIds = payloadObjectIds ?? Array.Empty<Guid>();
        }
    }

    /// <summary>
    /// Optional deterministic fault injection seam for scene mutation regression tests.
    /// </summary>
    public static class SceneMutationFaults
    {
        /// <summary>
        /// Fault injection callback. Return true to fail at the supplied stage.
        /// </summary>
        public static Func<Guid, string, bool> Injector { get; set; }

        internal static bool ShouldFail(Guid transactionId, string stage)
        {
            return Injector?.Invoke(transactionId, stage) ?? false;
        }
    }

    /// <summary>
    /// Deterministic injectable Scene save failure seam for regression tests.
    /// </summary>
    public static class SceneSaveFaults
    {
        /// <summary>
        /// Save fault callback. Return true to reject the save before native serialization or file I/O.
        /// </summary>
        public static Func<Guid, string, bool> Injector { get; set; }

        internal static bool ShouldFail(Scene scene)
        {
            return scene != null && (Injector?.Invoke(scene.ID, scene.Path) ?? false);
        }
    }
}
