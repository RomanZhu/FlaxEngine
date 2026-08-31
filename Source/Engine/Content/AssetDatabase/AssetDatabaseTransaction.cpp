// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabaseTransaction.h"
#include "SourceAssetDatabase.h"

namespace
{
    template<typename T, typename Predicate>
    void RemoveRows(Array<T>& rows, Predicate predicate)
    {
        for (int32 i = rows.Count() - 1; i >= 0; i--)
        {
            if (predicate(rows[i]))
                rows.RemoveAtKeepOrder(i);
        }
    }
}

AssetDatabaseTransaction::AssetDatabaseTransaction(SourceAssetDatabase* owner, const SourceAssetDatabaseState& state)
    : _owner(owner)
    , _baseRevision(state.Database.CurrentRevision)
    , _state(state)
{
}

uint64 AssetDatabaseTransaction::GetBaseRevision() const
{
    return _baseRevision;
}

const SourceAssetDatabaseState& AssetDatabaseTransaction::GetState() const
{
    return _state;
}

const AssetChangeSet& AssetDatabaseTransaction::GetChanges() const
{
    return _changes;
}

bool AssetDatabaseTransaction::IsCompleted() const
{
    return _completed;
}

void AssetDatabaseTransaction::SetLastCompleteScanId(uint64 scanId)
{
    ASSERT(!_completed);
    _state.Database.LastCompleteScanId = scanId;
}

void AssetDatabaseTransaction::SetImporterRegistryGeneration(uint64 generation)
{
    ASSERT(!_completed);
    _state.Database.ImporterRegistryGeneration = generation;
}

void AssetDatabaseTransaction::UpsertSource(const SourceAssetRow& source)
{
    ASSERT(!_completed);
    SourceAssetRow value = source;
    const uint64 revision = _baseRevision + 1;
    for (SourceAssetRow& current : _state.Sources)
    {
        if (current.AssetGuid != value.AssetGuid)
            continue;
        value.FirstSeenRevision = current.FirstSeenRevision;
        value.LastSeenRevision = revision;
        const bool sourceChanged = current.SourceHash != value.SourceHash;
        const bool metadataChanged = current.MetaHash != value.MetaHash;
        const bool moved = current.CanonicalPath != value.CanonicalPath;
        const bool statusChanged = current.Status != value.Status;
        value.LastModifiedRevision = sourceChanged || metadataChanged || moved ? revision : current.LastModifiedRevision;
        if (sourceChanged)
            _changes.SourceChanged.Add({ value.AssetGuid, current.SourceHash, value.SourceHash });
        if (metadataChanged)
            _changes.MetadataChanged.Add({ value.AssetGuid, current.MetaHash, value.MetaHash });
        if (moved)
            _changes.Moved.Add({ value.AssetGuid, current.Path, value.Path });
        if (statusChanged)
            _changes.StatusChanged.Add({ value.AssetGuid, current.Status, value.Status });
        current = MoveTemp(value);
        return;
    }
    value.FirstSeenRevision = revision;
    value.LastSeenRevision = revision;
    value.LastModifiedRevision = revision;
    _state.Sources.Add(value);
    _changes.Added.Add({ value.AssetGuid, value.Path });
}

void AssetDatabaseTransaction::RemoveSource(const Guid& assetGuid)
{
    ASSERT(!_completed);
    for (int32 i = 0; i < _state.Sources.Count(); i++)
    {
        if (_state.Sources[i].AssetGuid == assetGuid)
        {
            _changes.Removed.Add({ assetGuid, _state.Sources[i].Path });
            _state.Sources.RemoveAtKeepOrder(i);
            break;
        }
    }
    RemoveRows(_state.Objects, [&](const SourceAssetObjectRow& value) { return value.AssetGuid == assetGuid; });
    RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& value)
    {
        return value.OwnerAssetGuid == assetGuid || value.TargetAssetGuid == assetGuid;
    });
    RemoveRows(_state.Publications, [&](const SourceAssetPublicationRow& value) { return value.AssetGuid == assetGuid; });
    RemoveRows(_state.Diagnostics, [&](const SourceAssetDiagnosticRow& value) { return value.AssetGuid == assetGuid; });
}

void AssetDatabaseTransaction::ReplaceObjects(const Guid& assetGuid, const Array<SourceAssetObjectRow>& objects)
{
    ASSERT(!_completed);
    Array<SourceAssetObjectRow> previous;
    for (const SourceAssetObjectRow& value : _state.Objects)
    {
        if (value.AssetGuid == assetGuid)
            previous.Add(value);
    }
    RemoveRows(_state.Objects, [&](const SourceAssetObjectRow& value) { return value.AssetGuid == assetGuid; });
    AssetObjectsChangedChange change;
    change.AssetGuid = assetGuid;
    const uint64 revision = _baseRevision + 1;
    bool changed = previous.Count() != objects.Count();
    for (SourceAssetObjectRow value : objects)
    {
        value.AssetGuid = assetGuid;
        const SourceAssetObjectRow* old = nullptr;
        for (const SourceAssetObjectRow& candidate : previous)
        {
            if (candidate.LocalFileId == value.LocalFileId)
            {
                old = &candidate;
                break;
            }
        }
        if (old)
        {
            value.FirstSeenRevision = old->FirstSeenRevision;
            const bool objectChanged = old->ObjectGuid != value.ObjectGuid || old->StableIdentifier != value.StableIdentifier ||
                old->SubAssetKey != value.SubAssetKey || old->TypeName != value.TypeName || old->DisplayName != value.DisplayName ||
                old->IsMain != value.IsMain || old->IsRemoved != value.IsRemoved || old->Status != value.Status ||
                old->ObjectMetadata != value.ObjectMetadata;
            value.LastModifiedRevision = objectChanged ? revision : old->LastModifiedRevision;
            changed |= objectChanged;
        }
        else
        {
            value.FirstSeenRevision = revision;
            value.LastModifiedRevision = revision;
            changed = true;
        }
        value.LastSeenRevision = revision;
        _state.Objects.Add(value);
        change.LocalFileIds.Add(value.LocalFileId);
    }
    if (changed)
        _changes.ObjectsChanged.Add(MoveTemp(change));
}

void AssetDatabaseTransaction::ReplaceDependencies(const Guid& assetGuid, const StringView& targetId, const Array<SourceAssetDependencyRow>& dependencies)
{
    ASSERT(!_completed);
    RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& value)
    {
        return value.OwnerAssetGuid == assetGuid && value.TargetId == targetId;
    });
    for (SourceAssetDependencyRow value : dependencies)
    {
        value.OwnerAssetGuid = assetGuid;
        value.TargetId = targetId;
        _state.Dependencies.Add(MoveTemp(value));
    }
}

void AssetDatabaseTransaction::ReplaceDependencies(const Guid& assetGuid, int64 localFileId, const StringView& targetId,
    const Array<SourceAssetDependencyRow>& dependencies)
{
    ASSERT(!_completed);
    RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& value)
    {
        return value.OwnerAssetGuid == assetGuid && value.OwnerLocalFileId == localFileId && value.TargetId == targetId;
    });
    for (SourceAssetDependencyRow value : dependencies)
    {
        value.OwnerAssetGuid = assetGuid;
        value.OwnerLocalFileId = localFileId;
        value.TargetId = targetId;
        _state.Dependencies.Add(MoveTemp(value));
    }
}

void AssetDatabaseTransaction::UpsertPublication(const SourceAssetPublicationRow& publication)
{
    ASSERT(!_completed);
    SourceAssetPublicationRow value = publication;
    for (SourceAssetPublicationRow& current : _state.Publications)
    {
        if (current.AssetGuid == value.AssetGuid && current.LocalFileId == value.LocalFileId && current.TargetId == value.TargetId)
        {
            current = value;
            _changes.Imported.Add({ value.AssetGuid, value.LocalFileId, value.TargetId, value.Artifact });
            return;
        }
    }
    _state.Publications.Add(value);
    _changes.Imported.Add({ value.AssetGuid, value.LocalFileId, value.TargetId, value.Artifact });
}

void AssetDatabaseTransaction::ReplaceDiagnostics(const Guid& assetGuid, const Array<SourceAssetDiagnosticRow>& diagnostics)
{
    ASSERT(!_completed);
    RemoveRows(_state.Diagnostics, [&](const SourceAssetDiagnosticRow& value) { return value.AssetGuid == assetGuid && value.IsActive; });
    uint32 activeCount = 0;
    uint64 nextDiagnosticId = 1;
    for (const SourceAssetDiagnosticRow& existing : _state.Diagnostics)
    {
        if (existing.DiagnosticId >= nextDiagnosticId)
            nextDiagnosticId = existing.DiagnosticId + 1;
    }
    for (SourceAssetDiagnosticRow value : diagnostics)
    {
        value.AssetGuid = assetGuid;
        value.Diagnostic.AssetGuid = assetGuid;
        if (value.DiagnosticId == 0)
            value.DiagnosticId = nextDiagnosticId++;
        if (value.CreatedRevision == 0)
            value.CreatedRevision = _baseRevision + 1;
        activeCount += value.IsActive ? 1 : 0;
        _state.Diagnostics.Add(MoveTemp(value));
    }
    _changes.DiagnosticsChanged.Add({ assetGuid, activeCount });
}

bool AssetDatabaseTransaction::Commit(AssetPipelineDiagnostic& diagnostic)
{
    if (_completed || !_owner)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = AssetPipelineDiagnosticCode::SnapshotInvalid;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.Message = TEXT("Source asset database transaction is no longer active.");
        return true;
    }
    const bool failed = _owner->Commit(*this, diagnostic);
    if (!failed)
        _completed = true;
    return failed;
}

void AssetDatabaseTransaction::Rollback()
{
    _completed = true;
    _owner = nullptr;
}
