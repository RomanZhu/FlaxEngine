// Copyright (c) Wojciech Figat. All rights reserved.

#include "AssetDatabase.h"
#include "Engine/Threading/Threading.h"

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
}

AssetDatabase& AssetDatabase::Get()
{
    static AssetDatabase instance;
    return instance;
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

bool AssetDatabase::PublishFullSnapshot(const Array<AssetRecord>& records, AssetPipelineDiagnostic& diagnostic)
{
    diagnostic = AssetPipelineDiagnostic();
    Dictionary<Guid, AssetRecord> nextRecords;
    Dictionary<String, Guid> nextMainByPath;
    Dictionary<Guid, Array<Guid>> nextSubAssetsBySource;
    Dictionary<String, Array<Guid>> nextRecordsByProcessor;
    Dictionary<AssetRecordStatus, Array<Guid>> nextRecordsByStatus;
    Dictionary<Guid, Array<Guid>> nextDependantsByBuildInput;
    Dictionary<Guid, Array<Guid>> nextReferencersByRuntimeReference;

    nextRecords.EnsureCapacity(records.Count());
    for (const AssetRecord& input : records)
    {
        if (!input.ID.IsValid() || !input.SourceAssetID.IsValid())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, input.SourcePath.Get(), TEXT("Asset database record has an invalid identity."));
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

    AssetDatabaseChangeBatch changes;
    {
        ScopeLock lock(_locker);
        changes.Revision = _revision + 1;
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
        _mainByPath = MoveTemp(nextMainByPath);
        _subAssetsBySource = MoveTemp(nextSubAssetsBySource);
        _recordsByProcessor = MoveTemp(nextRecordsByProcessor);
        _recordsByStatus = MoveTemp(nextRecordsByStatus);
        _dependantsByBuildInput = MoveTemp(nextDependantsByBuildInput);
        _referencersByRuntimeReference = MoveTemp(nextReferencersByRuntimeReference);
        _revision = changes.Revision;
    }
    Changed(changes);
    return false;
}

void AssetDatabase::Clear()
{
    Array<AssetRecord> empty;
    AssetPipelineDiagnostic diagnostic;
    PublishFullSnapshot(empty, diagnostic);
}
