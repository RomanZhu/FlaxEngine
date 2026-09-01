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
    ImporterSettings,
};

/// <summary>Existing source selected by both canonical path and durable GUID.</summary>
struct FLAXENGINE_API AssetOperationTarget
{
    String SourcePath;
    Guid ExpectedGuid;
};

/// <summary>Exact durable metadata state captured by an importer settings editor.</summary>
struct FLAXENGINE_API AssetImporterSettingsRevision
{
    uint64 SourceRevision = 0;
    uint64 MetaSemanticHash = 0;
    String ImporterID;
    int32 StoredSettingsVersion = 0;
};

/// <summary>Exact hashes written by one controlled transaction for watcher suppression.</summary>
struct FLAXENGINE_API AssetOperationSelfWrite
{
    Guid TransactionId;
    String Path;
    ContentHash Content;
};

/// <summary>One staged metadata sidecar owned by a native canonical-registration batch.</summary>
API_STRUCT() struct FLAXENGINE_API AssetDefaultMetadataBatchEntry
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetDefaultMetadataBatchEntry);

    API_FIELD() Guid AssetID = Guid::Empty;
    API_FIELD() String SourcePath;
    API_FIELD() String StagingPath;
    API_FIELD() bool ReplaceExistingMetadata = false;
};

enum class AssetDefaultMetadataBatchFailurePoint : byte
{
    None,
    AfterFirstMetadata,
    AfterFirstMetadataWithoutRollback,
};

/// <summary>Recoverable trash location returned by delete/trash operations.</summary>
API_STRUCT() struct FLAXENGINE_API AssetTrashRecord
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetTrashRecord);

    API_FIELD() Guid TransactionId = Guid::Empty;
    API_FIELD() Guid AssetGuid = Guid::Empty;
    API_FIELD() String OriginalSourcePath;
    API_FIELD() String OriginalMetaPath;
    API_FIELD() String TrashSourcePath;
    API_FIELD() String TrashMetaPath;
    API_FIELD() String OriginalFragmentsPath;
    API_FIELD() String TrashFragmentsPath;
};

/// <summary>One exact canonical Content entry selected for recoverable trash staging.</summary>
API_STRUCT() struct FLAXENGINE_API AssetTrashEntryRequest
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetTrashEntryRequest);

    API_FIELD() String SourcePath;
    API_FIELD() Guid ExpectedAssetGuid = Guid::Empty;
    API_FIELD() bool IsFolder = false;
};

/// <summary>Exact copy behavior for one native batch entry.</summary>
API_ENUM() enum class AssetCopyEntryKind : byte
{
    CanonicalAsset,
    File,
    Directory,
    MetadataSidecar,
};

/// <summary>One exact source copy in an all-or-none native batch. A selected directory is expanded recursively unless descendants are already flattened.</summary>
API_STRUCT() struct FLAXENGINE_API AssetCopyEntryRequest
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetCopyEntryRequest);

    API_FIELD() String SourcePath;
    API_FIELD() String DestinationPath;
    API_FIELD() Guid ExpectedAssetGuid = Guid::Empty;
    API_FIELD() AssetCopyEntryKind Kind = AssetCopyEntryKind::CanonicalAsset;
};

/// <summary>Bounded native batch controls. Cancellation is sampled during discovery and between entries.</summary>
struct FLAXENGINE_API AssetOperationBatchOptions
{
    int32 MaximumEntries = 4096;
    const bool* Cancel = nullptr;
};

/// <summary>Exact native batch progress when preparation or an applied entry fails.</summary>
struct FLAXENGINE_API AssetOperationBatchResult
{
    int32 TotalEntries = 0;
    int32 CompletedEntries = 0;
    int32 RolledBackEntries = 0;
    int32 FailureIndex = -1;
    String FailurePath;
    bool Cancelled = false;
};

/// <summary>Exact native-owned recovery paths for one staged Content entry.</summary>
API_STRUCT() struct FLAXENGINE_API AssetTrashFragment
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetTrashFragment);

    API_FIELD() String OriginalPath;
    API_FIELD() String TrashPath;
};

/// <summary>Exact native-owned recovery paths for one staged Content entry.</summary>
API_STRUCT() struct FLAXENGINE_API AssetTrashEntry
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetTrashEntry);

    API_FIELD() Guid AssetGuid = Guid::Empty;
    API_FIELD() String OriginalPath;
    API_FIELD() String TrashPath;
    API_FIELD() String OriginalMetaPath;
    API_FIELD() String TrashMetaPath;
    API_FIELD() Array<AssetTrashFragment> Fragments;
    API_FIELD() bool IsFolder = false;
};

/// <summary>One all-or-none native trash transaction retained by editor undo history.</summary>
API_STRUCT() struct FLAXENGINE_API AssetTrashBatch
{
    DECLARE_SCRIPTING_TYPE_MINIMAL(AssetTrashBatch);

    API_FIELD() Guid TransactionId = Guid::Empty;
    API_FIELD() Array<AssetTrashEntry> Entries;
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

    /// <summary>Rejects importer settings writes against a stale durable source row.</summary>
    virtual bool ValidateImporterSettingsRevision(const AssetOperationTarget& target,
        const AssetImporterSettingsRevision& expected, AssetPipelineDiagnostic& diagnostic) = 0;

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
    int32 _immediateTransactions = 0;
    Array<AssetOperationCommit> _pendingCommits;
    Array<AssetOperationSelfWrite> _selfWrites;

    bool NormalizeSource(const StringView& input, AssetPathPolicy::ProjectPath& result,
        AssetPipelineDiagnostic& diagnostic) const;
    bool ValidateExisting(const AssetOperationTarget& target, AssetPathPolicy::ProjectPath& normalized,
        AssetMeta& meta, AssetPipelineDiagnostic& diagnostic) const;
    bool AcquirePaths(const Array<String>& paths, Array<String>& acquired, AssetPipelineDiagnostic& diagnostic);
    void ReleasePaths(const Array<String>& acquired);
    bool BeginImmediateTransaction(const StringView& path, AssetPipelineDiagnostic& diagnostic);
    void EndImmediateTransaction();
    bool PublishCommit(AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic);
    bool CreateFromBytes(AssetOperationKind kind, const StringView& destination, const Span<byte>& sourceData,
        const AssetMeta& meta, AssetOperationCommit& commit, AssetPipelineDiagnostic& diagnostic);
    bool MoveExact(AssetOperationKind kind, const AssetOperationTarget& target, const StringView& destination,
        AssetTrashRecord* trash, AssetPipelineDiagnostic& diagnostic);
    bool CopyAssetInternal(const AssetOperationTarget& target, const StringView& destination, Guid& copiedGuid,
        AssetPipelineDiagnostic& diagnostic, AssetOperationCommit* deferredCommit,
        const Guid& requestedCopiedGuid = Guid::Empty);

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
    bool CopyAssets(const Array<AssetCopyEntryRequest>& requests, Array<Guid>& copiedGuids,
        AssetPipelineDiagnostic& diagnostic, const AssetOperationBatchOptions* options = nullptr,
        AssetOperationBatchResult* result = nullptr);
    bool WriteImporterSettings(const AssetOperationTarget& target, const AssetImporterSettingsRevision& expected,
        int32 settingsVersion, const StringAnsiView& settingsJson, AssetPipelineDiagnostic& diagnostic,
        AssetMetaWriteFailurePoint failurePoint = AssetMetaWriteFailurePoint::None, bool* wasChanged = nullptr,
        bool* wasConflict = nullptr);
    bool TrashAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
        AssetPipelineDiagnostic& diagnostic);
    bool DeleteAsset(const AssetOperationTarget& target, AssetTrashRecord& trash,
        AssetPipelineDiagnostic& diagnostic);
    bool RestoreAsset(const AssetTrashRecord& trash, AssetPipelineDiagnostic& diagnostic);
    bool TrashEntries(const Array<AssetTrashEntryRequest>& requests, AssetTrashBatch& trash,
        AssetPipelineDiagnostic& diagnostic, const AssetOperationBatchOptions* options = nullptr,
        AssetOperationBatchResult* result = nullptr);
    bool RestoreEntries(const AssetTrashBatch& trash, AssetPipelineDiagnostic& diagnostic);
    bool DiscardTrash(const AssetTrashBatch& trash, AssetPipelineDiagnostic& diagnostic);

    bool IsAssetEditing() const;
    void StartAssetEditing();
    bool StopAssetEditing(AssetPipelineDiagnostic& diagnostic);
    void RegisterSelfWrite(const StringView& path, const ContentHash& content);
    void DrainSelfWrites(Array<AssetOperationSelfWrite>& result);
};
