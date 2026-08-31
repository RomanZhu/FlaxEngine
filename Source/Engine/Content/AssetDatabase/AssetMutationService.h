// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMeta.h"
#include "Engine/Core/Delegate.h"

/// <summary>Source-tree operation performed by the native mutation backend.</summary>
enum class AssetMutationOperation : byte
{
    Validate,
    CreateAsset,
    PublishExternal,
    RegisterExisting,
    CreateFolder,
    Copy,
    Move,
    Rename,
    DeleteToRecovery,
    ReplaceContents,
    ReplaceAsset,
    Recover,
};

/// <summary>Stable failure classification returned to native and managed facades.</summary>
enum class AssetMutationFailure : byte
{
    None,
    InvalidSource,
    MissingSource,
    MissingMetadata,
    InvalidDestination,
    DestinationCollision,
    PathCycle,
    PathBusy,
    PermissionDenied,
    LockedStorage,
    UnsupportedCrossVolumeMove,
    VerificationFailure,
    CopyFailed,
    MoveFailed,
    DeleteFailed,
    InvalidMetadata,
    JournalFailure,
    CallbackRejected,
    CallbackHandledInvalidState,
    DatabaseCommitFailed,
    RecoveryRequired,
};

/// <summary>Decision supplied by the editor modification callback bridge.</summary>
enum class AssetMutationDecision : byte
{
    /// <summary>The native service performs the proposed mutation.</summary>
    Allow,
    /// <summary>The callback rejected the mutation.</summary>
    Deny,
    /// <summary>The native service performs the mutation using ReplacementPath after revalidation.</summary>
    AllowWithReplacementPath,
    /// <summary>A trusted integration completed the mutation through the same transaction protocol.</summary>
    AlreadyHandled,
};

/// <summary>Invocation mode included in every modification callback request.</summary>
enum class AssetMutationInvocation : byte
{
    Interactive,
    Headless,
    Migration,
};

/// <summary>Immutable request passed to pre-mutation decision hooks.</summary>
struct FLAXENGINE_API AssetMutationDecisionContext
{
    Guid TransactionID;
    Guid SessionID;
    AssetMutationOperation Operation = AssetMutationOperation::Validate;
    String SourcePath;
    String DestinationPath;
    bool IsDirectory = false;
    bool CancellationRequested = false;
    AssetMutationInvocation Invocation = AssetMutationInvocation::Interactive;
};

/// <summary>Typed modification callback response.</summary>
struct FLAXENGINE_API AssetMutationDecisionResult
{
    AssetMutationDecision Decision = AssetMutationDecision::Allow;
    String ReplacementPath;
    String Message;
};

/// <summary>Structured result of a source plus adjacent-metadata transaction.</summary>
struct FLAXENGINE_API AssetMutationResult
{
    bool Succeeded = false;
    bool HandledByCallback = false;
    bool RequiresRecovery = false;
    AssetMutationFailure Failure = AssetMutationFailure::None;
    Guid TransactionID;
    Guid AssetID;
    String SourcePath;
    String DestinationPath;
    String RecoveryPath;
    String Message;
    Array<String> ChangedPaths;
};

using AssetMutationDecisionHook = Function<AssetMutationDecisionResult(const AssetMutationDecisionContext&)>;
using AssetMutationCommittedHook = Function<void(const AssetMutationResult&)>;
using AssetMutationDatabaseCommitHook = Function<bool(const AssetMutationResult&)>;

/// <summary>
/// Owns atomic filesystem mutations for canonical sources and their adjacent .meta sidecars.
/// The configured database hook reconciles source state before the durable commit marker is published.
/// </summary>
class FLAXENGINE_API AssetMutationService
{
public:
    /// <param name="projectRoot">Absolute project root used to resolve Content/... paths.</param>
    /// <param name="contentRoot">Absolute canonical source root.</param>
    /// <param name="journalRoot">Durable active-journal directory, normally under Library.</param>
    /// <param name="recoveryRoot">Durable delete-recovery directory.</param>
    AssetMutationService(const StringView& projectRoot, const StringView& contentRoot, const StringView& journalRoot, const StringView& recoveryRoot,
        AssetMutationInvocation invocation = AssetMutationInvocation::Interactive);

    /// <summary>Optional editor callback bridge invoked after native preflight while paths are locked.</summary>
    AssetMutationDecisionHook DecisionHook;

    /// <summary>Optional notification invoked after a fully verified commit.</summary>
    AssetMutationCommittedHook CommittedHook;

    /// <summary>
    /// Commits the already-published filesystem view into the source database. Returns true on failure.
    /// When it fails the service rolls the filesystem back and invokes it again to reconcile the old view.
    /// </summary>
    AssetMutationDatabaseCommitHook DatabaseCommitHook;

    /// <summary>Validates a planned operation without taking a decision hook or changing the filesystem.</summary>
    bool Validate(AssetMutationOperation operation, const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result) const;

    /// <summary>Creates a folder and folder metadata as one journaled operation.</summary>
    bool CreateFolder(const StringView& path, AssetMutationResult& result);

    /// <summary>Creates a folder with caller-provided canonical metadata as one journaled operation.</summary>
    bool CreateFolder(const StringView& path, const AssetMeta& meta, AssetMutationResult& result);

    /// <summary>Creates an authored source file and its metadata as one journaled operation.</summary>
    bool CreateAsset(const StringView& path, const StringAnsiView& sourceContents, const AssetMeta& meta, AssetMutationResult& result);

    /// <summary>Publishes an external file and caller-prepared metadata as one journaled source pair.</summary>
    bool PublishExternal(const StringView& externalSourcePath, const StringView& destinationPath, const AssetMeta& meta,
        bool replaceExisting, AssetMutationResult& result);

    /// <summary>Creates or replaces metadata for an existing source entry without rewriting source bytes.</summary>
    bool RegisterExisting(const StringView& sourcePath, const AssetMeta& meta, bool replaceExistingMetadata,
        AssetMutationResult& result);

    /// <summary>Copies a source/meta pair, recursively assigning new GUIDs for a folder copy.</summary>
    bool Copy(const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result);

    /// <summary>Copies source/meta pairs under one durable journal and commit marker.</summary>
    bool CopyBatch(const Array<String>& sourcePaths, const Array<String>& destinationPaths, AssetMutationResult& result);

    /// <summary>Moves a source/meta pair on the same volume while preserving identity.</summary>
    bool Move(const StringView& sourcePath, const StringView& destinationPath, AssetMutationResult& result);

    /// <summary>Moves source/meta pairs under one durable journal while preserving every identity.</summary>
    bool MoveBatch(const Array<String>& sourcePaths, const Array<String>& destinationPaths, AssetMutationResult& result);

    /// <summary>Renames a source/meta pair, including case-only renames.</summary>
    bool Rename(const StringView& sourcePath, const StringView& newName, AssetMutationResult& result);

    /// <summary>Moves a source/meta pair into durable recovery storage.</summary>
    bool DeleteToRecovery(const StringView& sourcePath, AssetMutationResult& result);

    /// <summary>Moves source/meta pairs into durable recovery storage under one journal and commit marker.</summary>
    bool DeleteToRecoveryBatch(const Array<String>& sourcePaths, AssetMutationResult& result);

    /// <summary>Atomically replaces source bytes while retaining the adjacent metadata identity.</summary>
    bool ReplaceContents(const StringView& sourcePath, const StringView& replacementPath, AssetMutationResult& result);

    /// <summary>Atomically replaces source bytes while retaining the adjacent metadata identity.</summary>
    bool ReplaceContents(const StringView& sourcePath, const StringAnsiView& sourceContents, AssetMutationResult& result);

    /// <summary>Atomically replaces authored source bytes and metadata while preserving file identity.</summary>
    bool ReplaceAsset(const StringView& sourcePath, const StringAnsiView& sourceContents, const AssetMeta& meta, AssetMutationResult& result);

    /// <summary>Restores a pair returned by DeleteToRecovery to a canonical Content path.</summary>
    bool Recover(const StringView& recoveryPath, const StringView& destinationPath, AssetMutationResult& result);

    /// <summary>Restores recovery source/meta pairs under one durable journal and commit marker.</summary>
    bool RecoverBatch(const Array<String>& recoveryPaths, const Array<String>& destinationPaths, AssetMutationResult& result);

    /// <summary>Resolves every interrupted journal to a verified old or new pair state.</summary>
    /// <returns>True when one or more journals could not be recovered safely.</returns>
    bool RecoverPending(Array<AssetMutationResult>& results);

    const String& GetProjectRoot() const { return _projectRoot; }
    const String& GetContentRoot() const { return _contentRoot; }
    const String& GetJournalRoot() const { return _journalRoot; }
    const String& GetRecoveryRoot() const { return _recoveryRoot; }
    const Guid& GetSessionID() const { return _sessionID; }
    AssetMutationInvocation GetInvocation() const { return _invocation; }

private:
    String _projectRoot;
    String _contentRoot;
    String _journalRoot;
    String _recoveryRoot;
    Guid _sessionID;
    AssetMutationInvocation _invocation;
};
