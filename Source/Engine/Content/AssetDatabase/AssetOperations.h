// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetMeta.h"
#include "AssetPath.h"
#include "AssetSourceRootRegistry.h"
#include "Engine/Content/Artifacts/ArtifactKey.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Core/NonCopyable.h"
#include "Engine/Core/Types/Span.h"
#include "Engine/Platform/ConditionVariable.h"
#include "Engine/Platform/CriticalSection.h"

enum class AssetOperationKind : byte
{
    Create,
    Import,
    Move,
    Rename,
    Copy,
    Trash,
    Delete,
    Restore,
};

/// <summary>Existing source selected by both canonical path and durable GUID.</summary>
struct FLAXENGINE_API AssetOperationTarget
{
    String SourcePath;
    Guid ExpectedGuid;
};

/// <summary>Exact hashes written by one controlled transaction for watcher suppression.</summary>
struct FLAXENGINE_API AssetOperationSelfWrite
{
    Guid TransactionId;
    String Path;
    ContentHash Content;
};

/// <summary>Recoverable trash location returned by delete/trash operations.</summary>
struct FLAXENGINE_API AssetTrashRecord
{
    Guid TransactionId;
    Guid AssetGuid;
    String OriginalSourcePath;
    String OriginalMetaPath;
    String TrashSourcePath;
    String TrashMetaPath;
};

/// <summary>Committed filesystem mutation passed to the database refresh boundary.</summary>
struct FLAXENGINE_API AssetOperationCommit
{
    Guid TransactionId;
    AssetOperationKind Kind = AssetOperationKind::Create;
    Guid AssetGuid;
    Guid SourceAssetGuid;
    String SourcePath;
    String DestinationPath;
    Array<AssetOperationSelfWrite> SelfWrites;
};

/// <summary>Injectable modification processor invoked before any source mutation is reserved.</summary>
class FLAXENGINE_API IAssetModificationProcessor
{
public:
    virtual ~IAssetModificationProcessor() = default;
    virtual bool ValidateOperation(AssetOperationKind kind, const AssetOperationTarget& target,
        const StringView& destination, AssetPipelineDiagnostic& diagnostic) = 0;
};

/// <summary>Injectable database boundary for copy cleanup and precise refresh.</summary>
class FLAXENGINE_API IAssetOperationDatabaseCallbacks
{
public:
    virtual ~IAssetOperationDatabaseCallbacks() = default;

    /// <summary>Clears copied diagnostics and artifact publications before independent import.</summary>
    virtual bool ClearCopiedState(const Guid& sourceGuid, const Guid& copiedGuid,
        AssetPipelineDiagnostic& diagnostic) = 0;

    /// <summary>Refreshes committed operations. Start/StopAssetEditing batches calls here.</summary>
    virtual bool RefreshCommitted(const Array<AssetOperationCommit>& commits,
        AssetPipelineDiagnostic& diagnostic) = 0;
};

/// <summary>Crash-recoverable source-plus-meta operations rooted in one project.</summary>
class FLAXENGINE_API AssetOperations : public NonCopyable
{
private:
    String _projectRoot;
    String _contentRoot;
    String _libraryRoot;
    String _transactionsRoot;
    String _trashRoot;
    AssetSourceRootRegistry _rootRegistry;
    bool _rootRegistryValid = false;
    AssetPipelineDiagnostic _rootRegistryDiagnostic;
    IAssetModificationProcessor& _modificationProcessor;
    IAssetOperationDatabaseCallbacks& _databaseCallbacks;

    mutable CriticalSection _stateLocker;
    ConditionVariable _locksChanged;
    HashSet<String> _lockedPaths;
    int32 _editingDepth = 0;
    Array<AssetOperationCommit> _pendingCommits;
    Array<AssetOperationSelfWrite> _selfWrites;

    bool NormalizeSource(const StringView& input, AssetPathPolicy::ProjectPath& result,
        AssetPipelineDiagnostic& diagnostic) const;
    bool ValidateExisting(const AssetOperationTarget& target, AssetPathPolicy::ProjectPath& normalized,
        AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const;
    bool AcquirePaths(const Array<String>& paths, Array<String>& acquired, AssetPipelineDiagnostic& diagnostic);
    void ReleasePaths(const Array<String>& acquired);
    bool PublishCommit(AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic);
    bool CreateFromBytes(AssetOperationKind kind, const StringView& destination, const Span<byte>& sourceData,
        const AssetMeta& meta, AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic);
    bool MoveExact(AssetOperationKind kind, const AssetOperationTarget& target, const StringView& destination,
        AssetTrashRecord* trash, AssetPipelineDiagnostic& diagnostic);

public:
    AssetOperations(const StringView& projectRoot, const StringView& contentRoot, const StringView& libraryRoot,
        IAssetModificationProcessor& modificationProcessor, IAssetOperationDatabaseCallbacks& databaseCallbacks);

    bool Initialize(AssetPipelineDiagnostic& diagnostic);
    bool RecoverIncompleteTransactions(Array<AssetPipelineDiagnostic>& diagnostics);

    bool CreateAsset(const StringView& destination, const Span<byte>& sourceData, const AssetMeta& meta,
        AssetPipelineDiagnostic& diagnostic);
    bool ImportAsset(const StringView& externalSource, const StringView& destination, const AssetMeta& meta,
        AssetPipelineDiagnostic& diagnostic);
    bool MoveAsset(const AssetOperationTarget& target, const StringView& destination,
        AssetPipelineDiagnostic& diagnostic);
    bool RenameAsset(const AssetOperationTarget& target, const StringView& newFileName,
        AssetPipelineDiagnostic& diagnostic);
    bool CopyAsset(const AssetOperationTarget& target, const StringView& destination, Guid& copiedGuid,
        AssetPipelineDiagnostic& diagnostic);
    bool TrashAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
        AssetPipelineDiagnostic& diagnostic);
    bool DeleteAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
        AssetPipelineDiagnostic& diagnostic);
    bool RestoreAsset(const AssetTrashRecord& trash, AssetPipelineDiagnostic& diagnostic);

    void StartAssetEditing();
    bool StopAssetEditing(AssetPipelineDiagnostic& diagnostic);
    void DrainSelfWrites(Array<AssetOperationSelfWrite>& result);
};
