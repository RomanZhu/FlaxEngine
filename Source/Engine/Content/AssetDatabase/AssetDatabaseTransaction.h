// Copyright (c) Wojciech Figat. All rights reserved.

#pragma once

#include "AssetChangeSet.h"
#include "AssetDatabaseSchema.h"

class SourceAssetDatabase;

enum class AssetDatabaseMutationKind : byte
{
    SetLastCompleteScanId,
    SetImporterRegistryGeneration,
    UpsertSource,
    RemoveSource,
    ReplaceObjects,
    ReplaceDependencies,
    UpsertPublication,
    ReplaceDiagnostics,
    UpsertImportTarget,
    ReplaceArtifactObjects,
    SetLabels,
    AppendFileJournal,
    UpsertRefreshSession,
    UpsertImportAttempt,
    UpsertCustomDependency,
    RemoveCustomDependency,
    ReplaceSnapshot,
};

struct AssetDatabaseMutation
{
    AssetDatabaseMutationKind Kind = AssetDatabaseMutationKind::UpsertSource;
    Guid Key = Guid::Empty;
    int64 LocalFileId = 0;
    uint64 Value = 0;
    String TargetId;
    ArtifactKey Artifact;
    Array<byte> Payload;
};

/// <summary>Affected rows captured before one in-process transaction mutation.</summary>
struct AssetDatabaseUndoEntry
{
    AssetDatabaseMutationKind Kind = AssetDatabaseMutationKind::UpsertSource;
    Guid Key = Guid::Empty;
    int64 LocalFileId = 0;
    bool AllLocalFileIds = false;
    uint64 Value = 0;
    String TargetId;
    ArtifactKey Artifact;
    SourceAssetDatabaseState Before;
};

/// <summary>Private mutable copy committed optimistically against one database revision.</summary>
class FLAXENGINE_API AssetDatabaseTransaction
{
    friend class SourceAssetDatabase;

private:
    SourceAssetDatabase* _owner = nullptr;
    uint64 _baseRevision = 0;
    SourceAssetDatabaseState _state;
    AssetChangeSet _changes;
    Array<AssetDatabaseMutation> _mutations;
    Array<AssetDatabaseUndoEntry> _undo;
    bool _completed = false;

    AssetDatabaseTransaction(SourceAssetDatabase* owner, SourceAssetDatabaseState&& state);
    AssetDatabaseUndoEntry& CaptureUndo(AssetDatabaseMutationKind kind, const Guid& key = Guid::Empty,
        int64 localFileId = 0, const StringView& targetId = StringView::Empty,
        const ArtifactKey& artifact = ArtifactKey(), bool allLocalFileIds = false);
    void RestoreUndo();

public:
    AssetDatabaseTransaction() = default;
    ~AssetDatabaseTransaction()
    {
        Rollback();
    }

    uint64 GetBaseRevision() const;
    const SourceAssetDatabaseState& GetState() const;
    const AssetChangeSet& GetChanges() const;
    bool IsCompleted() const;

    void SetChangeContext(const Guid& refreshId, uint32 pass);
    void SetLastCompleteScanId(uint64 scanId);
    void SetImporterRegistryGeneration(uint64 generation);
    void UpsertSource(const SourceAssetRow& source);
    void RemoveSource(const Guid& assetGuid);
    void ReplaceObjects(const Guid& assetGuid, const Array<SourceAssetObjectRow>& objects);
    void ReplaceDependencies(const Guid& assetGuid, const StringView& targetId, const Array<SourceAssetDependencyRow>& dependencies);
    void ReplaceDependencies(const Guid& assetGuid, int64 localFileId, const StringView& targetId,
        const Array<SourceAssetDependencyRow>& dependencies);
    void UpsertPublication(const SourceAssetPublicationRow& publication);
    void ReplaceDiagnostics(const Guid& assetGuid, const Array<SourceAssetDiagnosticRow>& diagnostics);
    void UpsertImportTarget(const SourceAssetImportTargetRow& target);
    void ReplaceArtifactObjects(const ArtifactKey& artifact, const Array<SourceArtifactObjectRow>& objects);
    void SetLabels(const Guid& assetGuid, const Array<String>& labels);
    void AppendFileJournal(const SourceFileJournalRow& entry);
    void UpsertRefreshSession(const SourceRefreshSessionRow& session);
    void UpsertImportAttempt(const SourceImportAttemptRow& attempt);
    void UpsertCustomDependency(const SourceCustomDependencyRow& dependency);
    void RemoveCustomDependency(const StringView& dependencyName);
    void ReplaceSnapshot(SourceAssetDatabaseState&& state, AssetChangeSet&& changes);

    /// <summary>Commits all tables and one change set atomically. Returns true on failure.</summary>
    bool Commit(AssetPipelineDiagnostic& diagnostic);
    void Rollback();
};
