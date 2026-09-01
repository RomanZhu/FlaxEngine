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

    void RecordMutation(Array<AssetDatabaseMutation>& mutations, AssetDatabaseMutationKind kind,
        const SourceAssetDatabaseState* payload = nullptr, const Guid& key = Guid::Empty, int64 localFileId = 0,
        const StringView& targetId = StringView::Empty, const ArtifactKey& artifact = ArtifactKey(), uint64 value = 0)
    {
        AssetDatabaseMutation mutation;
        mutation.Kind = kind;
        mutation.Key = key;
        mutation.LocalFileId = localFileId;
        mutation.TargetId = targetId;
        mutation.Artifact = artifact;
        mutation.Value = value;
        if (payload)
            payload->Serialize(mutation.Payload);
        mutations.Add(MoveTemp(mutation));
    }
}

AssetDatabaseTransaction::AssetDatabaseTransaction(SourceAssetDatabase* owner, SourceAssetDatabaseState&& state)
    : _owner(owner)
    , _baseRevision(state.Database.CurrentRevision)
    , _state(MoveTemp(state))
{
}

AssetDatabaseUndoEntry& AssetDatabaseTransaction::CaptureUndo(AssetDatabaseMutationKind kind, const Guid& key,
    int64 localFileId, const StringView& targetId, const ArtifactKey& artifact, bool allLocalFileIds)
{
    AssetDatabaseUndoEntry entry;
    entry.Kind = kind;
    entry.Key = key;
    entry.LocalFileId = localFileId;
    entry.AllLocalFileIds = allLocalFileIds;
    entry.TargetId = targetId;
    entry.Artifact = artifact;
    _undo.Add(MoveTemp(entry));
    return _undo.Last();
}

void AssetDatabaseTransaction::RestoreUndo()
{
    for (int32 i = _undo.Count() - 1; i >= 0; i--)
    {
        AssetDatabaseUndoEntry& undo = _undo[i];
        const SourceAssetDatabaseState& before = undo.Before;
        switch (undo.Kind)
        {
        case AssetDatabaseMutationKind::SetLastCompleteScanId:
            _state.Database.LastCompleteScanId = undo.Value;
            break;
        case AssetDatabaseMutationKind::SetImporterRegistryGeneration:
            _state.Database.ImporterRegistryGeneration = undo.Value;
            break;
        case AssetDatabaseMutationKind::UpsertSource:
            RemoveRows(_state.Sources, [&](const SourceAssetRow& row) { return row.AssetGuid == undo.Key; });
            _state.Sources.Add(before.Sources);
            break;
        case AssetDatabaseMutationKind::RemoveSource:
            RemoveRows(_state.Sources, [&](const SourceAssetRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.Objects, [&](const SourceAssetObjectRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& row) { return row.OwnerAssetGuid == undo.Key || row.TargetAssetGuid == undo.Key; });
            RemoveRows(_state.Publications, [&](const SourceAssetPublicationRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.Diagnostics, [&](const SourceAssetDiagnosticRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.Labels, [&](const SourceAssetLabelRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.ImportAttempts, [&](const SourceImportAttemptRow& row) { return row.AssetGuid == undo.Key; });
            _state.Sources.Add(before.Sources);
            _state.Objects.Add(before.Objects);
            _state.Dependencies.Add(before.Dependencies);
            _state.Publications.Add(before.Publications);
            _state.Diagnostics.Add(before.Diagnostics);
            _state.ArtifactObjects.Add(before.ArtifactObjects);
            _state.Labels.Add(before.Labels);
            _state.ImportAttempts.Add(before.ImportAttempts);
            break;
        case AssetDatabaseMutationKind::ReplaceObjects:
            RemoveRows(_state.Objects, [&](const SourceAssetObjectRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& row) { return row.OwnerAssetGuid == undo.Key || row.TargetAssetGuid == undo.Key; });
            RemoveRows(_state.Publications, [&](const SourceAssetPublicationRow& row) { return row.AssetGuid == undo.Key; });
            RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& row) { return row.AssetGuid == undo.Key; });
            _state.Objects.Add(before.Objects);
            _state.Dependencies.Add(before.Dependencies);
            _state.Publications.Add(before.Publications);
            _state.ArtifactObjects.Add(before.ArtifactObjects);
            break;
        case AssetDatabaseMutationKind::ReplaceDependencies:
            RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& row)
            {
                return row.OwnerAssetGuid == undo.Key && row.TargetId == undo.TargetId &&
                    (undo.AllLocalFileIds || row.OwnerLocalFileId == undo.LocalFileId);
            });
            _state.Dependencies.Add(before.Dependencies);
            break;
        case AssetDatabaseMutationKind::UpsertPublication:
            RemoveRows(_state.Publications, [&](const SourceAssetPublicationRow& row)
            {
                return row.AssetGuid == undo.Key && row.LocalFileId == undo.LocalFileId && row.TargetId == undo.TargetId;
            });
            _state.Publications.Add(before.Publications);
            break;
        case AssetDatabaseMutationKind::ReplaceDiagnostics:
            RemoveRows(_state.Diagnostics, [&](const SourceAssetDiagnosticRow& row) { return row.AssetGuid == undo.Key; });
            _state.Diagnostics.Add(before.Diagnostics);
            break;
        case AssetDatabaseMutationKind::UpsertImportTarget:
            RemoveRows(_state.ImportTargets, [&](const SourceAssetImportTargetRow& row) { return row.TargetId == undo.TargetId; });
            _state.ImportTargets.Add(before.ImportTargets);
            break;
        case AssetDatabaseMutationKind::ReplaceArtifactObjects:
            RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& row) { return row.Artifact == undo.Artifact; });
            _state.ArtifactObjects.Add(before.ArtifactObjects);
            break;
        case AssetDatabaseMutationKind::SetLabels:
            RemoveRows(_state.Labels, [&](const SourceAssetLabelRow& row) { return row.AssetGuid == undo.Key; });
            _state.Labels.Add(before.Labels);
            break;
        case AssetDatabaseMutationKind::AppendFileJournal:
            _state.FileJournal.Resize(static_cast<int32>(undo.Value));
            break;
        case AssetDatabaseMutationKind::UpsertRefreshSession:
            RemoveRows(_state.RefreshSessions, [&](const SourceRefreshSessionRow& row) { return row.RefreshId == undo.Key; });
            _state.RefreshSessions.Add(before.RefreshSessions);
            break;
        case AssetDatabaseMutationKind::UpsertImportAttempt:
            RemoveRows(_state.ImportAttempts, [&](const SourceImportAttemptRow& row) { return row.AttemptId == undo.Key; });
            _state.ImportAttempts.Add(before.ImportAttempts);
            break;
        case AssetDatabaseMutationKind::UpsertCustomDependency:
        case AssetDatabaseMutationKind::RemoveCustomDependency:
            RemoveRows(_state.CustomDependencies, [&](const SourceCustomDependencyRow& row) { return row.DependencyName == undo.TargetId; });
            _state.CustomDependencies.Add(before.CustomDependencies);
            break;
        case AssetDatabaseMutationKind::ReplaceSnapshot:
            _state = MoveTemp(undo.Before);
            break;
        }
    }
    _undo.Clear();
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

void AssetDatabaseTransaction::SetChangeContext(const Guid& refreshId, uint32 pass)
{
    ASSERT(!_completed);
    ASSERT(refreshId.IsValid() || pass == 0);
    _changes.RefreshId = refreshId;
    _changes.Pass = pass;
}

void AssetDatabaseTransaction::SetLastCompleteScanId(uint64 scanId)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::SetLastCompleteScanId);
    undo.Value = _state.Database.LastCompleteScanId;
    _state.Database.LastCompleteScanId = scanId;
    RecordMutation(_mutations, AssetDatabaseMutationKind::SetLastCompleteScanId, nullptr, Guid::Empty, 0,
        StringView::Empty, ArtifactKey(), scanId);
}

void AssetDatabaseTransaction::SetImporterRegistryGeneration(uint64 generation)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::SetImporterRegistryGeneration);
    undo.Value = _state.Database.ImporterRegistryGeneration;
    _state.Database.ImporterRegistryGeneration = generation;
    RecordMutation(_mutations, AssetDatabaseMutationKind::SetImporterRegistryGeneration, nullptr, Guid::Empty, 0,
        StringView::Empty, ArtifactKey(), generation);
}

void AssetDatabaseTransaction::UpsertSource(const SourceAssetRow& source)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertSource, source.AssetGuid);
    for (const SourceAssetRow& row : _state.Sources)
        if (row.AssetGuid == source.AssetGuid)
            undo.Before.Sources.Add(row);
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
        SourceAssetDatabaseState payload;
        payload.Database = _state.Database;
        payload.Sources.Add(current);
        RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertSource, &payload, current.AssetGuid);
        return;
    }
    value.FirstSeenRevision = revision;
    value.LastSeenRevision = revision;
    value.LastModifiedRevision = revision;
    _state.Sources.Add(value);
    _changes.Added.Add({ value.AssetGuid, value.Path });
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.Sources.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertSource, &payload, value.AssetGuid);
}

void AssetDatabaseTransaction::RemoveSource(const Guid& assetGuid)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::RemoveSource, assetGuid);
    for (const SourceAssetRow& row : _state.Sources) if (row.AssetGuid == assetGuid) undo.Before.Sources.Add(row);
    for (const SourceAssetObjectRow& row : _state.Objects) if (row.AssetGuid == assetGuid) undo.Before.Objects.Add(row);
    for (const SourceAssetDependencyRow& row : _state.Dependencies) if (row.OwnerAssetGuid == assetGuid || row.TargetAssetGuid == assetGuid) undo.Before.Dependencies.Add(row);
    for (const SourceAssetPublicationRow& row : _state.Publications) if (row.AssetGuid == assetGuid) undo.Before.Publications.Add(row);
    for (const SourceAssetDiagnosticRow& row : _state.Diagnostics) if (row.AssetGuid == assetGuid) undo.Before.Diagnostics.Add(row);
    for (const SourceArtifactObjectRow& row : _state.ArtifactObjects) if (row.AssetGuid == assetGuid) undo.Before.ArtifactObjects.Add(row);
    for (const SourceAssetLabelRow& row : _state.Labels) if (row.AssetGuid == assetGuid) undo.Before.Labels.Add(row);
    for (const SourceImportAttemptRow& row : _state.ImportAttempts) if (row.AssetGuid == assetGuid) undo.Before.ImportAttempts.Add(row);
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
    RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& value) { return value.AssetGuid == assetGuid; });
    RemoveRows(_state.Labels, [&](const SourceAssetLabelRow& value) { return value.AssetGuid == assetGuid; });
    RemoveRows(_state.ImportAttempts, [&](const SourceImportAttemptRow& value) { return value.AssetGuid == assetGuid; });
    RecordMutation(_mutations, AssetDatabaseMutationKind::RemoveSource, nullptr, assetGuid);
}

void AssetDatabaseTransaction::ReplaceObjects(const Guid& assetGuid, const Array<SourceAssetObjectRow>& objects)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceObjects, assetGuid);
    for (const SourceAssetObjectRow& row : _state.Objects) if (row.AssetGuid == assetGuid) undo.Before.Objects.Add(row);
    for (const SourceAssetDependencyRow& row : _state.Dependencies) if (row.OwnerAssetGuid == assetGuid || row.TargetAssetGuid == assetGuid) undo.Before.Dependencies.Add(row);
    for (const SourceAssetPublicationRow& row : _state.Publications) if (row.AssetGuid == assetGuid) undo.Before.Publications.Add(row);
    for (const SourceArtifactObjectRow& row : _state.ArtifactObjects) if (row.AssetGuid == assetGuid) undo.Before.ArtifactObjects.Add(row);
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
    const auto hasObject = [&](int64 localFileId)
    {
        for (const SourceAssetObjectRow& value : _state.Objects)
            if (value.AssetGuid == assetGuid && value.LocalFileId == localFileId)
                return true;
        return false;
    };
    RemoveRows(_state.Dependencies, [&](const SourceAssetDependencyRow& value)
    {
        return (value.OwnerAssetGuid == assetGuid && !hasObject(value.OwnerLocalFileId)) ||
            (value.TargetAssetGuid == assetGuid && value.TargetLocalFileId != 0 && !hasObject(value.TargetLocalFileId));
    });
    RemoveRows(_state.Publications, [&](const SourceAssetPublicationRow& value)
    {
        return value.AssetGuid == assetGuid && !hasObject(value.LocalFileId);
    });
    RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& value)
    {
        return value.AssetGuid == assetGuid && !hasObject(value.LocalFileId);
    });
    if (changed)
        _changes.ObjectsChanged.Add(MoveTemp(change));
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (const SourceAssetObjectRow& value : _state.Objects)
        if (value.AssetGuid == assetGuid)
            payload.Objects.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceObjects, &payload, assetGuid);
}

void AssetDatabaseTransaction::ReplaceDependencies(const Guid& assetGuid, const StringView& targetId, const Array<SourceAssetDependencyRow>& dependencies)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceDependencies, assetGuid, 0, targetId,
        ArtifactKey(), true);
    for (const SourceAssetDependencyRow& row : _state.Dependencies)
        if (row.OwnerAssetGuid == assetGuid && row.TargetId == targetId)
            undo.Before.Dependencies.Add(row);
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
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (const SourceAssetDependencyRow& value : _state.Dependencies)
        if (value.OwnerAssetGuid == assetGuid && value.TargetId == targetId)
            payload.Dependencies.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceDependencies, &payload, assetGuid, 0, targetId);
}

void AssetDatabaseTransaction::ReplaceDependencies(const Guid& assetGuid, int64 localFileId, const StringView& targetId,
    const Array<SourceAssetDependencyRow>& dependencies)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceDependencies, assetGuid, localFileId, targetId);
    for (const SourceAssetDependencyRow& row : _state.Dependencies)
        if (row.OwnerAssetGuid == assetGuid && row.OwnerLocalFileId == localFileId && row.TargetId == targetId)
            undo.Before.Dependencies.Add(row);
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
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (const SourceAssetDependencyRow& value : _state.Dependencies)
        if (value.OwnerAssetGuid == assetGuid && value.OwnerLocalFileId == localFileId && value.TargetId == targetId)
            payload.Dependencies.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceDependencies, &payload, assetGuid, localFileId, targetId);
}

void AssetDatabaseTransaction::UpsertPublication(const SourceAssetPublicationRow& publication)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertPublication, publication.AssetGuid,
        publication.LocalFileId, publication.TargetId);
    for (const SourceAssetPublicationRow& row : _state.Publications)
        if (row.AssetGuid == publication.AssetGuid && row.LocalFileId == publication.LocalFileId && row.TargetId == publication.TargetId)
            undo.Before.Publications.Add(row);
    SourceAssetPublicationRow value = publication;
    for (SourceAssetPublicationRow& current : _state.Publications)
    {
        if (current.AssetGuid == value.AssetGuid && current.LocalFileId == value.LocalFileId && current.TargetId == value.TargetId)
        {
            current = value;
            _changes.Imported.Add({ value.AssetGuid, value.LocalFileId, value.TargetId, value.Artifact });
            SourceAssetDatabaseState payload;
            payload.Database = _state.Database;
            payload.Publications.Add(value);
            RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertPublication, &payload, value.AssetGuid,
                value.LocalFileId, value.TargetId);
            return;
        }
    }
    _state.Publications.Add(value);
    _changes.Imported.Add({ value.AssetGuid, value.LocalFileId, value.TargetId, value.Artifact });
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.Publications.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertPublication, &payload, value.AssetGuid,
        value.LocalFileId, value.TargetId);
}

void AssetDatabaseTransaction::ReplaceDiagnostics(const Guid& assetGuid, const Array<SourceAssetDiagnosticRow>& diagnostics)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceDiagnostics, assetGuid);
    for (const SourceAssetDiagnosticRow& row : _state.Diagnostics)
        if (row.AssetGuid == assetGuid)
            undo.Before.Diagnostics.Add(row);
    RemoveRows(_state.Diagnostics, [&](const SourceAssetDiagnosticRow& value) { return value.AssetGuid == assetGuid && value.IsActive; });
    uint32 activeCount = 0;
    uint64 nextDiagnosticId = 1;
    for (const SourceAssetDiagnosticRow& existing : _state.Diagnostics)
    {
        if (existing.DiagnosticId >= nextDiagnosticId)
            nextDiagnosticId = existing.DiagnosticId + 1;
    }
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (SourceAssetDiagnosticRow value : diagnostics)
    {
        value.AssetGuid = assetGuid;
        value.Diagnostic.AssetGuid = assetGuid;
        if (value.DiagnosticId == 0)
            value.DiagnosticId = nextDiagnosticId++;
        if (value.CreatedRevision == 0)
            value.CreatedRevision = _baseRevision + 1;
        activeCount += value.IsActive ? 1 : 0;
        payload.Diagnostics.Add(value);
        _state.Diagnostics.Add(MoveTemp(value));
    }
    _changes.DiagnosticsChanged.Add({ assetGuid, activeCount });
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceDiagnostics, &payload, assetGuid);
}

void AssetDatabaseTransaction::UpsertImportTarget(const SourceAssetImportTargetRow& target)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertImportTarget, Guid::Empty, 0, target.TargetId);
    for (const SourceAssetImportTargetRow& row : _state.ImportTargets)
        if (row.TargetId == target.TargetId)
            undo.Before.ImportTargets.Add(row);
    for (SourceAssetImportTargetRow& current : _state.ImportTargets)
    {
        if (current.TargetId == target.TargetId)
        {
            current = target;
            SourceAssetDatabaseState payload;
            payload.Database = _state.Database;
            payload.ImportTargets.Add(current);
            RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertImportTarget, &payload, Guid::Empty, 0, target.TargetId);
            return;
        }
    }
    _state.ImportTargets.Add(target);
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.ImportTargets.Add(target);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertImportTarget, &payload, Guid::Empty, 0, target.TargetId);
}

void AssetDatabaseTransaction::ReplaceArtifactObjects(const ArtifactKey& artifact, const Array<SourceArtifactObjectRow>& objects)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceArtifactObjects, Guid::Empty, 0,
        StringView::Empty, artifact);
    for (const SourceArtifactObjectRow& row : _state.ArtifactObjects)
        if (row.Artifact == artifact)
            undo.Before.ArtifactObjects.Add(row);
    RemoveRows(_state.ArtifactObjects, [&](const SourceArtifactObjectRow& value) { return value.Artifact == artifact; });
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (SourceArtifactObjectRow value : objects)
    {
        value.Artifact = artifact;
        _state.ArtifactObjects.Add(value);
        payload.ArtifactObjects.Add(MoveTemp(value));
    }
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceArtifactObjects, &payload, Guid::Empty, 0,
        StringView::Empty, artifact);
}

void AssetDatabaseTransaction::SetLabels(const Guid& assetGuid, const Array<String>& labels)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::SetLabels, assetGuid);
    for (const SourceAssetLabelRow& row : _state.Labels)
        if (row.AssetGuid == assetGuid)
            undo.Before.Labels.Add(row);
    RemoveRows(_state.Labels, [&](const SourceAssetLabelRow& value) { return value.AssetGuid == assetGuid; });
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    for (const String& label : labels)
    {
        SourceAssetLabelRow row;
        row.AssetGuid = assetGuid;
        row.Label = label;
        _state.Labels.Add(row);
        payload.Labels.Add(MoveTemp(row));
    }
    RecordMutation(_mutations, AssetDatabaseMutationKind::SetLabels, &payload, assetGuid);
}

void AssetDatabaseTransaction::AppendFileJournal(const SourceFileJournalRow& entry)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::AppendFileJournal);
    undo.Value = _state.FileJournal.Count();
    SourceFileJournalRow value = entry;
    if (value.Sequence == 0)
        value.Sequence = _state.FileJournal.HasItems() ? _state.FileJournal.Last().Sequence + 1 : 1;
    _state.FileJournal.Add(value);
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.FileJournal.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::AppendFileJournal, &payload);
}

void AssetDatabaseTransaction::UpsertRefreshSession(const SourceRefreshSessionRow& session)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertRefreshSession, session.RefreshId);
    for (const SourceRefreshSessionRow& row : _state.RefreshSessions)
        if (row.RefreshId == session.RefreshId)
            undo.Before.RefreshSessions.Add(row);
    for (SourceRefreshSessionRow& current : _state.RefreshSessions)
    {
        if (current.RefreshId == session.RefreshId)
        {
            current = session;
            SourceAssetDatabaseState payload;
            payload.Database = _state.Database;
            payload.RefreshSessions.Add(current);
            RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertRefreshSession, &payload, session.RefreshId);
            return;
        }
    }
    _state.RefreshSessions.Add(session);
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.RefreshSessions.Add(session);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertRefreshSession, &payload, session.RefreshId);
}

void AssetDatabaseTransaction::UpsertImportAttempt(const SourceImportAttemptRow& attempt)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertImportAttempt, attempt.AttemptId);
    for (const SourceImportAttemptRow& row : _state.ImportAttempts)
        if (row.AttemptId == attempt.AttemptId)
            undo.Before.ImportAttempts.Add(row);
    for (SourceImportAttemptRow& current : _state.ImportAttempts)
    {
        if (current.AttemptId == attempt.AttemptId)
        {
            current = attempt;
            SourceAssetDatabaseState payload;
            payload.Database = _state.Database;
            payload.ImportAttempts.Add(current);
            RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertImportAttempt, &payload, attempt.AttemptId);
            return;
        }
    }
    _state.ImportAttempts.Add(attempt);
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.ImportAttempts.Add(attempt);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertImportAttempt, &payload, attempt.AttemptId);
}

void AssetDatabaseTransaction::UpsertCustomDependency(const SourceCustomDependencyRow& dependency)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::UpsertCustomDependency, Guid::Empty, 0,
        dependency.DependencyName);
    for (const SourceCustomDependencyRow& row : _state.CustomDependencies)
        if (row.DependencyName == dependency.DependencyName)
            undo.Before.CustomDependencies.Add(row);
    SourceCustomDependencyRow value = dependency;
    if (value.ModifiedRevision == 0)
        value.ModifiedRevision = _baseRevision + 1;
    for (SourceCustomDependencyRow& current : _state.CustomDependencies)
    {
        if (current.DependencyName == value.DependencyName)
        {
            current = value;
            SourceAssetDatabaseState payload;
            payload.Database = _state.Database;
            payload.CustomDependencies.Add(current);
            RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertCustomDependency, &payload, Guid::Empty, 0,
                value.DependencyName);
            return;
        }
    }
    _state.CustomDependencies.Add(value);
    SourceAssetDatabaseState payload;
    payload.Database = _state.Database;
    payload.CustomDependencies.Add(value);
    RecordMutation(_mutations, AssetDatabaseMutationKind::UpsertCustomDependency, &payload, Guid::Empty, 0,
        value.DependencyName);
}

void AssetDatabaseTransaction::RemoveCustomDependency(const StringView& dependencyName)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::RemoveCustomDependency, Guid::Empty, 0,
        dependencyName);
    for (const SourceCustomDependencyRow& row : _state.CustomDependencies)
        if (row.DependencyName == dependencyName)
            undo.Before.CustomDependencies.Add(row);
    RemoveRows(_state.CustomDependencies, [&](const SourceCustomDependencyRow& value)
    {
        return value.DependencyName == dependencyName;
    });
    RecordMutation(_mutations, AssetDatabaseMutationKind::RemoveCustomDependency, nullptr, Guid::Empty, 0, dependencyName);
}

void AssetDatabaseTransaction::ReplaceSnapshot(SourceAssetDatabaseState&& state, AssetChangeSet&& changes)
{
    ASSERT(!_completed);
    AssetDatabaseUndoEntry& undo = CaptureUndo(AssetDatabaseMutationKind::ReplaceSnapshot);
    undo.Before = MoveTemp(_state);
    state.Database.CurrentRevision = _baseRevision + 1;
    state.Database.CleanShutdown = false;
    _state = MoveTemp(state);
    _changes = MoveTemp(changes);
    _mutations.Clear();
    RecordMutation(_mutations, AssetDatabaseMutationKind::ReplaceSnapshot, &_state);
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
    if (_owner && !_completed)
    {
        RestoreUndo();
        _owner->Rollback(*this);
    }
    _completed = true;
    _owner = nullptr;
}
