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
    RecordMutation(_mutations, AssetDatabaseMutationKind::SetLastCompleteScanId, nullptr, Guid::Empty, 0,
        StringView::Empty, ArtifactKey(), scanId);
}

void AssetDatabaseTransaction::SetImporterRegistryGeneration(uint64 generation)
{
    ASSERT(!_completed);
    _state.Database.ImporterRegistryGeneration = generation;
    RecordMutation(_mutations, AssetDatabaseMutationKind::SetImporterRegistryGeneration, nullptr, Guid::Empty, 0,
        StringView::Empty, ArtifactKey(), generation);
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
            const bool objectChanged = old->StableIdentifier != value.StableIdentifier ||
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
    RemoveRows(_state.CustomDependencies, [&](const SourceCustomDependencyRow& value)
    {
        return value.DependencyName == dependencyName;
    });
    RecordMutation(_mutations, AssetDatabaseMutationKind::RemoveCustomDependency, nullptr, Guid::Empty, 0, dependencyName);
}

void AssetDatabaseTransaction::ReplaceSnapshot(SourceAssetDatabaseState&& state, AssetChangeSet&& changes)
{
    ASSERT(!_completed);
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
    _completed = true;
    _owner = nullptr;
}
