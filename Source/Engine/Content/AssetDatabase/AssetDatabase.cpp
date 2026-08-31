// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabase.h"
#include "Identity/AssetObjectId.h"
#include "Engine/Core/Collections/HashSet.h"
#include "Engine/Platform/StringUtils.h"
#include "Engine/Threading/Threading.h"
#include <algorithm>

namespace
{
    template<typename Key>
    void AddToIndex(Dictionary<Key, Array<Guid>>& index, const Key& key, const Guid& id)
    {
        Array<Guid>* values = index.TryGet(key);
        if (!values)
        {
            index.Add(key, Array<Guid>());
            values = index.TryGet(key);
        }
        values->Add(id);
    }

    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const StringView& path, const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::DatabaseScan;
        diagnostic.SourcePath = path;
        diagnostic.Message = message;
        return true;
    }

    void ResolveRecords(const Dictionary<Guid, AssetRecord>& records, const Array<Guid>* ids, Array<AssetRecord>& result)
    {
        result.Clear();
        if (!ids)
            return;
        result.EnsureCapacity(ids->Count());
        for (const Guid& id : *ids)
        {
            const AssetRecord* record = records.TryGet(id);
            if (record)
                result.Add(*record);
        }
    }

    ContentHash SemanticHash(uint64 value)
    {
        ContentHash result;
        result.Values[0] = (uint32)value;
        result.Values[1] = (uint32)(value >> 32);
        return result;
    }

    void DurableStateToRecords(const SourceAssetDatabaseState& state, Array<AssetRecord>& records)
    {
        records.Clear();
        Dictionary<Guid, const SourceAssetRow*> sources;
        Dictionary<AssetObjectId, Guid> objectGuids;
        for (const SourceAssetRow& source : state.Sources)
            sources.Add(source.AssetGuid, &source);
        for (const SourceAssetObjectRow& object : state.Objects)
            objectGuids.Add(AssetObjectId(AssetGuid(object.AssetGuid), object.LocalFileId), object.ObjectGuid);
        records.EnsureCapacity(state.Objects.Count());
        for (const SourceAssetObjectRow& object : state.Objects)
        {
            if (object.IsRemoved)
                continue;
            const SourceAssetRow* const* source = sources.TryGet(object.AssetGuid);
            if (!source)
                continue;
            AssetRecord record;
            record.ID = object.ObjectGuid;
            record.SourceAssetID = object.AssetGuid;
            record.LocalId = object.LocalFileId;
            record.TypeName = object.TypeName;
            record.CanonicalPath = CanonicalAssetPath((*source)->Path);
            record.SourcePath = SourceFilePath((*source)->Path);
            record.MetaPath = MetaFilePath((*source)->MetaPath);
            record.SubAsset = SubAssetKey(object.SubAssetKey);
            record.ProcessorID = (*source)->ImporterId;
            record.PortabilityKey = (*source)->PortabilityKey;
            record.MetaSemanticHash = (*source)->MetaSemanticHash;
            record.SourceKind = (*source)->SourceKind;
            record.Status = object.Status;
            record.DatabaseRevision = object.LastModifiedRevision;
            for (const SourceAssetDependencyRow& dependency : state.Dependencies)
            {
                if (dependency.OwnerAssetGuid != object.AssetGuid || dependency.OwnerLocalFileId != object.LocalFileId)
                    continue;
                Guid target = dependency.TargetAssetGuid;
                if (dependency.TargetLocalFileId != 0)
                {
                    const Guid* objectGuid = objectGuids.TryGet(AssetObjectId(AssetGuid(dependency.TargetAssetGuid), dependency.TargetLocalFileId));
                    target = objectGuid ? *objectGuid : Guid::Empty;
                }
                if (!target.IsValid())
                    continue;
                if (dependency.Kind == AssetDependencyKind::BuildInput)
                {
                    if (!record.BuildInputDependencies.Contains(target))
                        record.BuildInputDependencies.Add(target);
                }
                else if (dependency.Kind == AssetDependencyKind::RuntimeReference)
                {
                    if (!record.RuntimeReferences.Contains(target))
                        record.RuntimeReferences.Add(target);
                }
            }
            const auto lessGuid = [](const Guid& a, const Guid& b)
            {
                if (a.A != b.A)
                    return a.A < b.A;
                if (a.B != b.B)
                    return a.B < b.B;
                if (a.C != b.C)
                    return a.C < b.C;
                return a.D < b.D;
            };
            if (record.BuildInputDependencies.Count() > 1)
                std::sort(record.BuildInputDependencies.Get(), record.BuildInputDependencies.Get() + record.BuildInputDependencies.Count(), lessGuid);
            if (record.RuntimeReferences.Count() > 1)
                std::sort(record.RuntimeReferences.Get(), record.RuntimeReferences.Get() + record.RuntimeReferences.Count(), lessGuid);
            records.Add(MoveTemp(record));
        }
    }
}

AssetDatabase& AssetDatabase::Get()
{
    static AssetDatabase instance;
    return instance;
}

bool AssetDatabase::Open(const StringView& libraryPath, const Guid& projectId, AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    if (_sourceDatabase.Open(libraryPath, projectId, diagnostic))
        return true;
    RebuildCacheFromDurable();
    return false;
}

bool AssetDatabase::Close(AssetPipelineDiagnostic* diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    const bool failed = _sourceDatabase.Close(diagnostic);
    ScopeLock lock(_locker);
    _revision = 0;
    _records.Clear();
    _recordByObject.Clear();
    _mainByPath.Clear();
    _subAssetsBySource.Clear();
    _recordsByProcessor.Clear();
    _recordsByStatus.Clear();
    _dependantsByBuildInput.Clear();
    _referencersByRuntimeReference.Clear();
    return failed;
}

bool AssetDatabase::IsOpen() const
{
    return _sourceDatabase.IsOpen();
}

uint64 AssetDatabase::GetRevision() const
{
    ScopeLock lock(_locker);
    return _revision;
}

AssetDatabaseSnapshot AssetDatabase::GetSnapshot() const
{
    ScopeLock lock(_locker);
    AssetDatabaseSnapshot result;
    result.Revision = _revision;
    _records.GetValues(result.Records);
    return result;
}

bool AssetDatabase::TryGetRecord(const Guid& id, AssetRecord& result) const
{
    ScopeLock lock(_locker);
    const AssetRecord* record = _records.TryGet(id);
    if (!record)
        return false;
    result = *record;
    return true;
}

bool AssetDatabase::TryGetRecord(const AssetObjectId& id, AssetRecord& result) const
{
    ScopeLock lock(_locker);
    const Guid* backingId = _recordByObject.TryGet(id);
    const AssetRecord* record = backingId ? _records.TryGet(*backingId) : nullptr;
    if (!record)
        return false;
    result = *record;
    return true;
}

bool AssetDatabase::TryGetMainRecordByPath(const StringView& portabilityKey, AssetRecord& result) const
{
    ScopeLock lock(_locker);
    String key(portabilityKey);
    key = key.ToLower();
    key.Replace(TEXT('\\'), TEXT('/'));
    const Guid* id = _mainByPath.TryGet(key);
    const AssetRecord* record = id ? _records.TryGet(*id) : nullptr;
    if (!record)
        return false;
    result = *record;
    return true;
}

void AssetDatabase::GetSubAssets(const Guid& sourceId, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _subAssetsBySource.TryGet(sourceId), result);
}

void AssetDatabase::GetByProcessor(const StringView& processorId, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _recordsByProcessor.TryGet(String(processorId)), result);
}

void AssetDatabase::GetByStatus(AssetRecordStatus status, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _recordsByStatus.TryGet(status), result);
}

void AssetDatabase::GetBuildDependants(const Guid& inputId, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _dependantsByBuildInput.TryGet(inputId), result);
}

void AssetDatabase::GetRuntimeReferencers(const Guid& referencedId, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _referencersByRuntimeReference.TryGet(referencedId), result);
}

bool AssetDatabase::PublishCache(const Array<AssetRecord>& records, uint64 revision, AssetDatabaseChangeBatch& changes, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    Dictionary<Guid, AssetRecord> nextRecords;
    Dictionary<AssetObjectId, Guid> nextRecordByObject;
    HashSet<AssetObjectId> nextObjectIds;
    Dictionary<String, Guid> nextMainByPath;
    Dictionary<Guid, Array<Guid>> nextSubAssetsBySource;
    Dictionary<String, Array<Guid>> nextRecordsByProcessor;
    Dictionary<AssetRecordStatus, Array<Guid>> nextRecordsByStatus;
    Dictionary<Guid, Array<Guid>> nextDependantsByBuildInput;
    Dictionary<Guid, Array<Guid>> nextReferencersByRuntimeReference;

    nextRecords.EnsureCapacity(records.Count());
    for (const AssetRecord& input : records)
    {
        if (!input.ID.IsValid() || !input.SourceAssetID.IsValid() || input.LocalId <= 0)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, input.SourcePath.Get(), TEXT("Asset database record has an invalid identity."));
        if ((input.IsMainAsset() && input.LocalId != 1) || (!input.IsMainAsset() && input.LocalId == 1))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, input.SourcePath.Get(), TEXT("Main objects require local file ID 1 and subassets require a different positive ID."));
        const AssetObjectId objectId(AssetGuid(input.SourceAssetID), input.LocalId);
        if (!nextObjectIds.Add(objectId))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, input.SourcePath.Get(), TEXT("Asset database input repeats a GUID/local file ID identity."));
        if (nextRecords.ContainsKey(input.ID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, input.SourcePath.Get(), TEXT("Asset database input contains a duplicate GUID."));
        AssetRecord record = input;
        // The collision pass below is authoritative over the whole set being published, so a status
        // carried in from an earlier publish has to be dropped or a resolved collision never clears.
        if (record.Status == AssetRecordStatus::PathCollision)
            record.Status = AssetRecordStatus::Ready;
        if (record.PortabilityKey.IsEmpty())
            record.PortabilityKey = record.CanonicalPath.Get().ToLower();
        else
            record.PortabilityKey = record.PortabilityKey.ToLower();
        record.PortabilityKey.Replace(TEXT('\\'), TEXT('/'));
        if (record.IsMainAsset())
        {
            const Guid* existing = nextMainByPath.TryGet(record.PortabilityKey);
            if (existing)
            {
                record.Status = AssetRecordStatus::PathCollision;
                AssetRecord* other = nextRecords.TryGet(*existing);
                if (other)
                    other->Status = AssetRecordStatus::PathCollision;
            }
            else
            {
                nextMainByPath.Add(record.PortabilityKey, record.ID);
            }
        }
        else
        {
            AddToIndex(nextSubAssetsBySource, record.SourceAssetID, record.ID);
        }
        nextRecordByObject.Add(objectId, record.ID);
        nextRecords.Add(record.ID, MoveTemp(record));
    }
    for (const auto& entry : nextRecords)
    {
        AddToIndex(nextRecordsByProcessor, entry.Value.ProcessorID, entry.Key);
        AddToIndex(nextRecordsByStatus, entry.Value.Status, entry.Key);
        for (const Guid& dependency : entry.Value.BuildInputDependencies)
            AddToIndex(nextDependantsByBuildInput, dependency, entry.Key);
        for (const Guid& reference : entry.Value.RuntimeReferences)
            AddToIndex(nextReferencersByRuntimeReference, reference, entry.Key);
    }

    changes = AssetDatabaseChangeBatch();
    {
        ScopeLock lock(_locker);
        changes.Revision = revision;
        for (auto& entry : nextRecords)
        {
            const AssetRecord* previous = _records.TryGet(entry.Key);
            if (!previous)
            {
                entry.Value.DatabaseRevision = changes.Revision;
                changes.Added.Add(entry.Key);
            }
            else
            {
                const bool contentChanged = !previous->HasSameIdentityAndContent(entry.Value);
                const bool statusChanged = previous->Status != entry.Value.Status;
                entry.Value.DatabaseRevision = contentChanged || statusChanged ? changes.Revision : previous->DatabaseRevision;
                if (contentChanged)
                    changes.Changed.Add(entry.Key);
                if (statusChanged)
                    changes.StatusChanged.Add(entry.Key);
            }
        }
        for (const auto& entry : _records)
        {
            if (!nextRecords.ContainsKey(entry.Key))
                changes.Removed.Add(entry.Key);
        }
        _records = MoveTemp(nextRecords);
        _recordByObject = MoveTemp(nextRecordByObject);
        _mainByPath = MoveTemp(nextMainByPath);
        _subAssetsBySource = MoveTemp(nextSubAssetsBySource);
        _recordsByProcessor = MoveTemp(nextRecordsByProcessor);
        _recordsByStatus = MoveTemp(nextRecordsByStatus);
        _dependantsByBuildInput = MoveTemp(nextDependantsByBuildInput);
        _referencersByRuntimeReference = MoveTemp(nextReferencersByRuntimeReference);
        _revision = changes.Revision;
    }
    return false;
}

bool AssetDatabase::PublishFullSnapshot(const Array<AssetRecord>& records, AssetPipelineDiagnostic& diagnostic)
{
    Array<AssetPipelineDiagnostic> diagnostics;
    return PublishFullSnapshot(records, diagnostics, diagnostic);
}

bool AssetDatabase::PublishFullSnapshot(const Array<AssetRecord>& records, const Array<AssetPipelineDiagnostic>& diagnostics, AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    if (!_sourceDatabase.IsOpen())
    {
        AssetDatabaseChangeBatch changes;
        if (PublishCache(records, GetRevision() + 1, changes, diagnostic))
            return true;
        Changed(changes);
        return false;
    }

    std::unique_ptr<AssetDatabaseTransaction> transaction = _sourceDatabase.BeginTransaction();
    if (!transaction)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, StringView::Empty, TEXT("Cannot begin the authoritative source asset database transaction."));

    Dictionary<Guid, const AssetRecord*> firstBySource;
    Dictionary<Guid, const AssetRecord*> mainBySource;
    Dictionary<Guid, Guid> sourceByObject;
    Dictionary<Guid, int64> localIdByObject;
    Dictionary<Guid, Array<SourceAssetObjectRow>> objectsBySource;
    Dictionary<Guid, Array<SourceAssetDependencyRow>> dependenciesBySource;
    HashSet<Guid> incomingSources;
    for (const AssetRecord& record : records)
    {
        if (!record.ID.IsValid() || !record.SourceAssetID.IsValid() || record.LocalId <= 0)
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, record.SourcePath.Get(), TEXT("Asset database record has an invalid identity."));
        if (sourceByObject.ContainsKey(record.ID))
            return Fail(diagnostic, AssetPipelineDiagnosticCode::DuplicateGuid, record.SourcePath.Get(), TEXT("Asset database input contains a duplicate object GUID."));
        sourceByObject.Add(record.ID, record.SourceAssetID);
        localIdByObject.Add(record.ID, record.LocalId);
        incomingSources.Add(record.SourceAssetID);
        if (!firstBySource.ContainsKey(record.SourceAssetID))
            firstBySource.Add(record.SourceAssetID, &record);
        if (record.IsMainAsset())
        {
            const AssetRecord** existingMain = mainBySource.TryGet(record.SourceAssetID);
            if (existingMain)
                *existingMain = &record;
            else
                mainBySource.Add(record.SourceAssetID, &record);
        }
    }

    const Array<SourceAssetRow> currentSources = transaction->GetState().Sources;
    for (const SourceAssetRow& source : currentSources)
    {
        if (!incomingSources.Contains(source.AssetGuid))
            transaction->RemoveSource(source.AssetGuid);
    }

    for (auto sourceIt = incomingSources.Begin(); sourceIt.IsNotEnd(); ++sourceIt)
    {
        const Guid& sourceId = sourceIt->Item;
        const AssetRecord* const* mainPtr = mainBySource.TryGet(sourceId);
        const AssetRecord* const* firstPtr = firstBySource.TryGet(sourceId);
        const AssetRecord* sourceRecord = mainPtr ? *mainPtr : *firstPtr;
        SourceAssetRow source;
        source.AssetGuid = sourceId;
        source.Path = sourceRecord->SourcePath.Get();
        source.CanonicalPath = sourceRecord->CanonicalPath.Get();
        source.CanonicalPath.Replace(TEXT('\\'), TEXT('/'));
        source.MetaPath = sourceRecord->MetaPath.Get();
        source.CanonicalMetaPath = source.MetaPath;
        source.CanonicalMetaPath.Replace(TEXT('\\'), TEXT('/'));
        source.IsFolder = sourceRecord->SourceKind == AssetSourceKind::Folder;
        source.MetaHash = SemanticHash(sourceRecord->MetaSemanticHash);
        source.MetaSemanticHash = sourceRecord->MetaSemanticHash;
        source.ImporterId = sourceRecord->ProcessorID;
        source.PortabilityKey = sourceRecord->PortabilityKey;
        source.SourceKind = sourceRecord->SourceKind;
        source.Status = sourceRecord->Status;
        transaction->UpsertSource(source);
    }

    for (const AssetRecord& record : records)
    {
        Array<SourceAssetObjectRow>* objects = objectsBySource.TryGet(record.SourceAssetID);
        if (!objects)
        {
            objectsBySource.Add(record.SourceAssetID, Array<SourceAssetObjectRow>());
            objects = objectsBySource.TryGet(record.SourceAssetID);
        }
        SourceAssetObjectRow object;
        object.AssetGuid = record.SourceAssetID;
        object.ObjectGuid = record.ID;
        object.LocalFileId = record.LocalId;
        object.StableIdentifier = record.IsMainAsset() ? String(TEXT("main")) : String(record.SubAsset.Get());
        if (object.StableIdentifier.IsEmpty())
            object.StableIdentifier = StringUtils::ToString(record.LocalId);
        object.SubAssetKey = record.SubAsset.Get();
        object.TypeName = record.TypeName;
        object.IsMain = record.IsMainAsset();
        object.Status = record.Status;
        objects->Add(MoveTemp(object));

        Array<SourceAssetDependencyRow>* dependencies = dependenciesBySource.TryGet(record.SourceAssetID);
        if (!dependencies)
        {
            dependenciesBySource.Add(record.SourceAssetID, Array<SourceAssetDependencyRow>());
            dependencies = dependenciesBySource.TryGet(record.SourceAssetID);
        }
        for (const Guid& dependencyId : record.BuildInputDependencies)
        {
            SourceAssetDependencyRow dependency;
            dependency.OwnerAssetGuid = record.SourceAssetID;
            dependency.OwnerLocalFileId = record.LocalId;
            dependency.TargetId = TEXT("default");
            dependency.Kind = AssetDependencyKind::BuildInput;
            const Guid* targetSource = sourceByObject.TryGet(dependencyId);
            const int64* targetLocalId = localIdByObject.TryGet(dependencyId);
            dependency.TargetAssetGuid = targetSource ? *targetSource : dependencyId;
            dependency.TargetLocalFileId = targetLocalId ? *targetLocalId : 0;
            dependency.CustomDependency = dependencyId.ToString(Guid::FormatType::N);
            dependencies->Add(MoveTemp(dependency));
        }
        for (const Guid& dependencyId : record.RuntimeReferences)
        {
            SourceAssetDependencyRow dependency;
            dependency.OwnerAssetGuid = record.SourceAssetID;
            dependency.OwnerLocalFileId = record.LocalId;
            dependency.TargetId = TEXT("default");
            dependency.Kind = AssetDependencyKind::RuntimeReference;
            const Guid* targetSource = sourceByObject.TryGet(dependencyId);
            const int64* targetLocalId = localIdByObject.TryGet(dependencyId);
            dependency.TargetAssetGuid = targetSource ? *targetSource : dependencyId;
            dependency.TargetLocalFileId = targetLocalId ? *targetLocalId : 0;
            dependency.CustomDependency = dependencyId.ToString(Guid::FormatType::N);
            dependencies->Add(MoveTemp(dependency));
        }
    }
    for (auto sourceIt = incomingSources.Begin(); sourceIt.IsNotEnd(); ++sourceIt)
    {
        const Guid& sourceId = sourceIt->Item;
        const Array<SourceAssetObjectRow>* objects = objectsBySource.TryGet(sourceId);
        const Array<SourceAssetDependencyRow>* dependencies = dependenciesBySource.TryGet(sourceId);
        const Array<SourceAssetObjectRow> emptyObjects;
        const Array<SourceAssetDependencyRow> emptyDependencies;
        transaction->ReplaceObjects(sourceId, objects ? *objects : emptyObjects);
        transaction->ReplaceDependencies(sourceId, TEXT("default"), dependencies ? *dependencies : emptyDependencies);
    }

    Dictionary<Guid, Array<SourceAssetDiagnosticRow>> diagnosticsBySource;
    Array<SourceAssetDiagnosticRow> unattributedDiagnostics;
    for (const AssetPipelineDiagnostic& value : diagnostics)
    {
        Guid sourceId = Guid::Empty;
        const Guid* mapped = sourceByObject.TryGet(value.AssetGuid);
        if (mapped)
            sourceId = *mapped;
        else if (incomingSources.Contains(value.AssetGuid))
            sourceId = value.AssetGuid;
        SourceAssetDiagnosticRow row;
        row.AssetGuid = sourceId;
        row.Diagnostic = value;
        row.Diagnostic.AssetGuid = sourceId;
        if (!sourceId.IsValid())
        {
            unattributedDiagnostics.Add(MoveTemp(row));
            continue;
        }
        Array<SourceAssetDiagnosticRow>* rows = diagnosticsBySource.TryGet(sourceId);
        if (!rows)
        {
            diagnosticsBySource.Add(sourceId, Array<SourceAssetDiagnosticRow>());
            rows = diagnosticsBySource.TryGet(sourceId);
        }
        rows->Add(MoveTemp(row));
    }
    for (auto sourceIt = incomingSources.Begin(); sourceIt.IsNotEnd(); ++sourceIt)
    {
        const Guid& sourceId = sourceIt->Item;
        const Array<SourceAssetDiagnosticRow>* rows = diagnosticsBySource.TryGet(sourceId);
        const Array<SourceAssetDiagnosticRow> empty;
        transaction->ReplaceDiagnostics(sourceId, rows ? *rows : empty);
    }
    transaction->ReplaceDiagnostics(Guid::Empty, unattributedDiagnostics);
    if (transaction->Commit(diagnostic))
        return true;

    Array<AssetRecord> durableRecords;
    DurableStateToRecords(_sourceDatabase.Read().GetState(), durableRecords);
    AssetDatabaseChangeBatch changes;
    if (PublishCache(durableRecords, _sourceDatabase.GetRevision(), changes, diagnostic))
        return true;
    Changed(changes);
    return false;
}

void AssetDatabase::RebuildCacheFromDurable(AssetDatabaseChangeBatch* outputChanges)
{
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    if (!snapshot.IsValid())
        return;
    Array<AssetRecord> records;
    DurableStateToRecords(snapshot.GetState(), records);
    AssetDatabaseChangeBatch changes;
    AssetPipelineDiagnostic diagnostic;
    if (!PublishCache(records, snapshot.GetRevision(), changes, diagnostic) && outputChanges)
        *outputChanges = MoveTemp(changes);
}

AssetDatabaseReadSnapshot AssetDatabase::GetDurableSnapshot() const
{
    return _sourceDatabase.Read();
}

bool AssetDatabase::ReadChangesAfter(uint64 revision, Array<AssetChangeSet>& result, bool& requiresSnapshot, AssetPipelineDiagnostic& diagnostic) const
{
    return _sourceDatabase.ReadChangesAfter(revision, result, requiresSnapshot, diagnostic);
}

bool AssetDatabase::RecordPublication(const SourceAssetPublicationRow& publication, const Array<SourceAssetDependencyRow>& dependencies,
    AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    std::unique_ptr<AssetDatabaseTransaction> transaction = _sourceDatabase.BeginTransaction();
    if (!transaction)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, StringView::Empty, TEXT("Cannot begin a publication transaction."));
    transaction->ReplaceDependencies(publication.AssetGuid, publication.LocalFileId, publication.TargetId, dependencies);
    transaction->UpsertPublication(publication);
    if (transaction->Commit(diagnostic))
        return true;
    AssetDatabaseChangeBatch changes;
    RebuildCacheFromDurable(&changes);
    Changed(changes);
    return false;
}

void AssetDatabase::Clear()
{
    Array<AssetRecord> empty;
    AssetPipelineDiagnostic diagnostic;
    PublishFullSnapshot(empty, diagnostic);
}
