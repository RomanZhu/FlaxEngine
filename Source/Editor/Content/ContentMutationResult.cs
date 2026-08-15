// Copyright (c) Wojciech Figat. All rights reserved.

using System;

namespace FlaxEditor.Content
{
    internal enum ContentMutationFailure
    {
        None,
        InvalidSource,
        MissingSource,
        InvalidDestination,
        DestinationCollision,
        PathCycle,
        PermissionDenied,
        LockedStorage,
        UnsupportedCrossVolumeMove,
        VerificationFailure,
        CopyFailed,
        MoveFailed,
        DeleteFailed,
        UnsupportedLink,
        JournalFailure,
        RollbackFailure,
        RecoveryRequired,
    }

    internal readonly struct ContentMutationResult
    {
        public readonly bool Succeeded;
        public readonly ContentMutationFailure Failure;
        public readonly string SourcePath;
        public readonly string DestinationPath;
        public readonly string Message;
        public readonly bool CreatedDestination;
        public readonly bool RequiresRecovery;
        public readonly Guid TransactionId;
        public readonly string[] CompletedPaths;
        public readonly string[] RolledBackPaths;

        private ContentMutationResult(bool succeeded, ContentMutationFailure failure, string sourcePath, string destinationPath, string message, bool createdDestination, bool requiresRecovery, Guid transactionId, string[] completedPaths, string[] rolledBackPaths)
        {
            Succeeded = succeeded;
            Failure = failure;
            SourcePath = sourcePath;
            DestinationPath = destinationPath;
            Message = message;
            CreatedDestination = createdDestination;
            RequiresRecovery = requiresRecovery;
            TransactionId = transactionId;
            CompletedPaths = completedPaths ?? Array.Empty<string>();
            RolledBackPaths = rolledBackPaths ?? Array.Empty<string>();
        }

        public static ContentMutationResult Success(string sourcePath, string destinationPath, Guid transactionId = default, string[] completedPaths = null)
        {
            return new ContentMutationResult(true, ContentMutationFailure.None, sourcePath, destinationPath, null, true, false, transactionId, completedPaths, null);
        }

        public static ContentMutationResult Prepared(string sourcePath, string destinationPath, Guid transactionId = default)
        {
            return new ContentMutationResult(true, ContentMutationFailure.None, sourcePath, destinationPath, null, false, false, transactionId, null, null);
        }

        public static ContentMutationResult Fail(ContentMutationFailure failure, string sourcePath, string destinationPath, string message, bool requiresRecovery = false, Guid transactionId = default, string[] completedPaths = null, string[] rolledBackPaths = null)
        {
            return new ContentMutationResult(false, failure, sourcePath, destinationPath, message, false, requiresRecovery, transactionId, completedPaths, rolledBackPaths);
        }
    }
}
