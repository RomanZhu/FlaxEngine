// Copyright (c) Wojciech Figat. All rights reserved.

using System;
using System.Collections.Generic;
using System.IO;

namespace FlaxEditor.Content
{
    internal enum ContentMutationOperationKind
    {
        Create,
        Copy,
        Move,
        Rename,
        Delete,
        ImportOutput,
        Restore,
    }

    internal enum ContentMutationPathRole
    {
        Main,
        Descendant,
        SceneFragments,
        MetadataSidecar,
        Temporary,
        UndoTrash,
        ReplacementBackup,
    }

    internal enum ContentMutationEntryState
    {
        Prepared,
        Committing,
        Committed,
        RollingBack,
        RolledBack,
        Failed,
    }

    internal sealed class ContentMutationEntry
    {
        public string SourcePath;
        public string DestinationPath;
        public ContentMutationPathRole Role;
        public bool IsDirectory;
        public bool AllowEquivalentDestination;
        public bool SourceProducedByTransaction;
        public bool DestinationParentProducedByTransaction;
        public bool DestinationReleasedByTransaction;
        public bool AllowExistingDestination;
        public bool SourceRequired = true;
        public long SourceLength = -1;
        public DateTime SourceWriteTimeUtc;
        public bool SourceWasAsset;
        public bool AssetCloneExpected;
        public Guid SourceAssetId;
        public string SourceAssetType;
        public ContentMutationEntryState State;

        public ContentMutationEntry()
        {
        }

        public ContentMutationEntry(string sourcePath, string destinationPath, ContentMutationPathRole role, bool isDirectory, bool allowEquivalentDestination = false)
        {
            SourcePath = ContentMutationPathUtils.Normalize(sourcePath);
            DestinationPath = ContentMutationPathUtils.Normalize(destinationPath);
            Role = role;
            IsDirectory = isDirectory;
            AllowEquivalentDestination = allowEquivalentDestination;
            State = ContentMutationEntryState.Prepared;
        }
    }

    internal sealed class ContentMutationPlan
    {
        public Guid Id = Guid.NewGuid();
        public ContentMutationOperationKind Operation;
        public DateTime CreatedUtc = DateTime.UtcNow;
        public List<ContentMutationEntry> Entries = new List<ContentMutationEntry>();

        public ContentMutationPlan()
        {
        }

        public ContentMutationPlan(ContentMutationOperationKind operation)
        {
            Operation = operation;
        }

        public ContentMutationResult Preflight()
        {
            if (Entries == null || Entries.Count == 0)
                return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, null, null, "The mutation plan contains no entries.", transactionId: Id);

            var destinations = new HashSet<string>(ContentMutationPathUtils.Comparer);
            for (int i = 0; i < Entries.Count; i++)
            {
                var entry = Entries[i];
                entry.SourcePath = ContentMutationPathUtils.Normalize(entry.SourcePath);
                entry.DestinationPath = ContentMutationPathUtils.Normalize(entry.DestinationPath);
                if (entry.SourceRequired && (string.IsNullOrEmpty(entry.SourcePath) || (!entry.SourceProducedByTransaction && !ContentMutationPathUtils.Exists(entry.SourcePath))))
                    return ContentMutationResult.Fail(ContentMutationFailure.MissingSource, entry.SourcePath, entry.DestinationPath, $"Mutation source '{entry.SourcePath}' does not exist.", transactionId: Id);
                if (entry.SourceRequired && !entry.SourceProducedByTransaction && ContentMutationPathUtils.ContainsReparsePoint(entry.SourcePath, entry.IsDirectory))
                    return ContentMutationResult.Fail(ContentMutationFailure.UnsupportedLink, entry.SourcePath, entry.DestinationPath, $"Mutation source '{entry.SourcePath}' contains an unsupported filesystem link.", transactionId: Id);
                if (string.IsNullOrEmpty(entry.DestinationPath))
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, entry.SourcePath, entry.DestinationPath, "The mutation destination is invalid.", transactionId: Id);
                if (!ContentMutationPathUtils.TryValidateDestinationPath(entry.DestinationPath, out var invalidPathMessage))
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, entry.SourcePath, entry.DestinationPath, invalidPathMessage, transactionId: Id);
                if (entry.SourceRequired && !entry.SourceProducedByTransaction && entry.IsDirectory != Directory.Exists(entry.SourcePath))
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidSource, entry.SourcePath, entry.DestinationPath, $"Mutation source '{entry.SourcePath}' has an unexpected filesystem type.", transactionId: Id);
                if ((Operation == ContentMutationOperationKind.Move || Operation == ContentMutationOperationKind.Rename) &&
                    !ContentMutationPathUtils.IsSameVolume(entry.SourcePath, entry.DestinationPath))
                    return ContentMutationResult.Fail(ContentMutationFailure.UnsupportedCrossVolumeMove, entry.SourcePath, entry.DestinationPath, "Cross-volume Content moves are not supported by the atomic move backend.", transactionId: Id);

                var equivalent = ContentMutationPathUtils.AreEquivalent(entry.SourcePath, entry.DestinationPath);
                if (!destinations.Add(entry.DestinationPath))
                    return ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, entry.SourcePath, entry.DestinationPath, $"Multiple mutation entries target '{entry.DestinationPath}'.", transactionId: Id);
                if (ContentMutationPathUtils.Exists(entry.DestinationPath) && !entry.AllowExistingDestination && !entry.DestinationReleasedByTransaction && !(equivalent && entry.AllowEquivalentDestination))
                    return ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, entry.SourcePath, entry.DestinationPath, $"Mutation destination '{entry.DestinationPath}' already exists.", transactionId: Id);

                var parent = Path.GetDirectoryName(entry.DestinationPath);
                if (string.IsNullOrEmpty(parent) || (!entry.DestinationParentProducedByTransaction && !Directory.Exists(parent)))
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, entry.SourcePath, entry.DestinationPath, $"Mutation destination parent '{parent}' does not exist.", transactionId: Id);
                if (entry.IsDirectory && !equivalent && ContentMutationPathUtils.IsWithinRoot(entry.DestinationPath, entry.SourcePath))
                    return ContentMutationResult.Fail(ContentMutationFailure.PathCycle, entry.SourcePath, entry.DestinationPath, "A folder cannot be moved or copied into itself or a descendant.", transactionId: Id);

                try
                {
                    if (!entry.SourceRequired || entry.SourceProducedByTransaction)
                    {
                        entry.SourceLength = -1;
                        entry.SourceWriteTimeUtc = default;
                    }
                    else if (!entry.IsDirectory)
                    {
                        var info = new FileInfo(entry.SourcePath);
                        entry.SourceLength = info.Length;
                        entry.SourceWriteTimeUtc = info.LastWriteTimeUtc;
                        if (TryGetAssetIdentity(entry.SourcePath, out var assetId, out var assetType))
                        {
                            entry.SourceWasAsset = true;
                            entry.SourceAssetId = assetId;
                            entry.SourceAssetType = assetType;
                        }
                    }
                    else
                    {
                        entry.SourceWriteTimeUtc = Directory.GetLastWriteTimeUtc(entry.SourcePath);
                    }
                }
                catch (UnauthorizedAccessException ex)
                {
                    return ContentMutationResult.Fail(ContentMutationFailure.PermissionDenied, entry.SourcePath, entry.DestinationPath, ex.Message, transactionId: Id);
                }
                catch (IOException ex)
                {
                    return ContentMutationResult.Fail(ContentMutationFailure.LockedStorage, entry.SourcePath, entry.DestinationPath, ex.Message, transactionId: Id);
                }
            }

            return ContentMutationResult.Prepared(Entries[0].SourcePath, Entries[0].DestinationPath, Id);
        }

        public ContentMutationResult VerifyBeforeCommit(int[] entryIndices)
        {
            if (entryIndices == null || entryIndices.Length == 0)
                return ContentMutationResult.Prepared(Entries[0].SourcePath, Entries[0].DestinationPath, Id);

            for (int i = 0; i < entryIndices.Length; i++)
            {
                var entry = Entries[entryIndices[i]];
                if (entry.SourceRequired && (!ContentMutationPathUtils.Exists(entry.SourcePath) || entry.IsDirectory != Directory.Exists(entry.SourcePath)))
                    return ContentMutationResult.Fail(ContentMutationFailure.MissingSource, entry.SourcePath, entry.DestinationPath, $"Mutation source '{entry.SourcePath}' changed after preflight.", transactionId: Id);
                if (entry.SourceRequired && !entry.SourceProducedByTransaction)
                {
                    try
                    {
                        if (entry.IsDirectory)
                        {
                            if (Directory.GetLastWriteTimeUtc(entry.SourcePath) != entry.SourceWriteTimeUtc)
                                return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, entry.SourcePath, entry.DestinationPath, $"Mutation source folder '{entry.SourcePath}' changed after preflight.", transactionId: Id);
                        }
                        else
                        {
                            var info = new FileInfo(entry.SourcePath);
                            if (info.Length != entry.SourceLength || info.LastWriteTimeUtc != entry.SourceWriteTimeUtc)
                                return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, entry.SourcePath, entry.DestinationPath, $"Mutation source file '{entry.SourcePath}' changed after preflight.", transactionId: Id);
                            var isAsset = TryGetAssetIdentity(entry.SourcePath, out var assetId, out var assetType);
                            if (isAsset != entry.SourceWasAsset || isAsset && (assetId != entry.SourceAssetId || assetType != entry.SourceAssetType))
                                return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, entry.SourcePath, entry.DestinationPath, $"Mutation source asset identity '{entry.SourcePath}' changed after preflight.", transactionId: Id);
                        }
                    }
                    catch (Exception ex)
                    {
                        return ContentMutationResult.Fail(ContentMutationFailure.VerificationFailure, entry.SourcePath, entry.DestinationPath, ex.Message, transactionId: Id);
                    }
                }

                var equivalent = ContentMutationPathUtils.AreEquivalent(entry.SourcePath, entry.DestinationPath);
                if (ContentMutationPathUtils.Exists(entry.DestinationPath) && !entry.AllowExistingDestination && !(equivalent && entry.AllowEquivalentDestination))
                    return ContentMutationResult.Fail(ContentMutationFailure.DestinationCollision, entry.SourcePath, entry.DestinationPath, $"Mutation destination '{entry.DestinationPath}' appeared after preflight.", transactionId: Id);
                var parent = Path.GetDirectoryName(entry.DestinationPath);
                if (string.IsNullOrEmpty(parent) || (!entry.DestinationParentProducedByTransaction && !Directory.Exists(parent)))
                    return ContentMutationResult.Fail(ContentMutationFailure.InvalidDestination, entry.SourcePath, entry.DestinationPath, $"Mutation destination parent '{parent}' changed after preflight.", transactionId: Id);
            }
            return ContentMutationResult.Prepared(Entries[entryIndices[0]].SourcePath, Entries[entryIndices[0]].DestinationPath, Id);
        }

        private static bool TryGetAssetIdentity(string path, out Guid id, out string type)
        {
            try
            {
                if (FlaxEngine.Content.GetAssetInfo(path, out var assetInfo))
                {
                    id = assetInfo.ID;
                    type = assetInfo.TypeName;
                    return true;
                }
            }
            catch (DllNotFoundException)
            {
                // Pure managed test runners do not host the native engine. Filesystem identity
                // remains available there; live Editor transactions also capture asset identity.
            }
            catch (EntryPointNotFoundException)
            {
            }
            id = Guid.Empty;
            type = null;
            return false;
        }
    }
}
