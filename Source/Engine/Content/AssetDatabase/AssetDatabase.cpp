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

    String NormalizeIndexKey(const StringView& value)
    {
        String result(value);
        result = result.ToLower();
        result.Replace(TEXT('\\'), TEXT('/'));
        while (result.Length() > 1 && result.EndsWith('/'))
            result.Remove(result.Length() - 1, 1);
        return result;
    }

    String GetSearchText(const AssetRecord& record)
    {
        String result(StringUtils::GetFileNameWithoutExtension(record.SourcePath.Get()));
        if (record.DisplayName.HasChars())
            result += TEXT(" ") + record.DisplayName;
        if (record.SubAsset.Get().HasChars())
            result += TEXT(" ") + String(record.SubAsset.Get());
        result += TEXT(" ") + record.TypeName;
        return result.ToLower();
    }

    void AddSearchGrams(Dictionary<String, Array<Guid>>& index, const AssetRecord& record)
    {
        const String text = GetSearchText(record);
        for (int32 length = 1; length <= 3; length++)
        {
            for (int32 i = 0; i + length <= text.Length(); i++)
            {
                const String gram(text.Get() + i, length);
                Array<Guid>* values = index.TryGet(gram);
                if (!values)
                {
                    index.Add(gram, Array<Guid>());
                    values = index.TryGet(gram);
                }
                if (!values->Contains(record.ID))
                    values->Add(record.ID);
            }
        }
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
        Dictionary<Guid, Array<String>> labels;
        for (const SourceAssetRow& source : state.Sources)
            sources.Add(source.AssetGuid, &source);
        for (const SourceAssetLabelRow& label : state.Labels)
        {
            Array<String>* values = labels.TryGet(label.AssetGuid);
            if (!values)
            {
                labels.Add(label.AssetGuid, Array<String>());
                values = labels.TryGet(label.AssetGuid);
            }
            values->Add(label.Label);
        }
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
            record.DisplayName = object.DisplayName;
            record.ProcessorID = (*source)->ImporterId;
            record.PortabilityKey = (*source)->PortabilityKey;
            record.MetaSemanticHash = (*source)->MetaSemanticHash;
            const Array<String>* sourceLabels = labels.TryGet(object.AssetGuid);
            if (sourceLabels)
                record.Labels = *sourceLabels;
            record.SourceKind = (*source)->SourceKind;
            record.Status = object.Status;
            record.DatabaseRevision = object.LastModifiedRevision;
            for (const SourceAssetDependencyRow& dependency : state.Dependencies)
            {
                if (dependency.OwnerAssetGuid != object.AssetGuid || dependency.OwnerLocalFileId != object.LocalFileId)
                    continue;
                const AssetObjectId target(AssetGuid(dependency.TargetAssetGuid),
                    dependency.TargetLocalFileId != 0 ? dependency.TargetLocalFileId : 1);
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
            const auto lessObject = [](const AssetObjectId& a, const AssetObjectId& b)
            {
                const Guid& left = a.Asset.Value;
                const Guid& right = b.Asset.Value;
                if (left.A != right.A)
                    return left.A < right.A;
                if (left.B != right.B)
                    return left.B < right.B;
                if (left.C != right.C)
                    return left.C < right.C;
                if (left.D != right.D)
                    return left.D < right.D;
                return a.LocalId < b.LocalId;
            };
            if (record.BuildInputDependencies.Count() > 1)
                std::sort(record.BuildInputDependencies.Get(), record.BuildInputDependencies.Get() + record.BuildInputDependencies.Count(), lessObject);
            if (record.RuntimeReferences.Count() > 1)
                std::sort(record.RuntimeReferences.Get(), record.RuntimeReferences.Get() + record.RuntimeReferences.Count(), lessObject);
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
    _recordsByType.Clear();
    _recordsByLabel.Clear();
    _recordsBySortedPath.Clear();
    _recordsBySearchGram.Clear();
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
    ResolveRecords(_records, _recordsByProcessor.TryGet(NormalizeIndexKey(processorId)), result);
}

void AssetDatabase::GetByStatus(AssetRecordStatus status, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _recordsByStatus.TryGet(status), result);
}

void AssetDatabase::GetBuildDependants(const Guid& inputId, Array<AssetRecord>& result) const
{
    AssetRecord input;
    const AssetObjectId object = TryGetRecord(inputId, input)
        ? AssetObjectId(AssetGuid(input.SourceAssetID), input.LocalId)
        : AssetObjectId::Main(AssetGuid(inputId));
    GetBuildDependants(object, result);
}

void AssetDatabase::GetBuildDependants(const AssetObjectId& input, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _dependantsByBuildInput.TryGet(input), result);
}

void AssetDatabase::GetRuntimeReferencers(const Guid& referencedId, Array<AssetRecord>& result) const
{
    AssetRecord referenced;
    const AssetObjectId object = TryGetRecord(referencedId, referenced)
        ? AssetObjectId(AssetGuid(referenced.SourceAssetID), referenced.LocalId)
        : AssetObjectId::Main(AssetGuid(referencedId));
    GetRuntimeReferencers(object, result);
}

void AssetDatabase::GetRuntimeReferencers(const AssetObjectId& referenced, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    ResolveRecords(_records, _referencersByRuntimeReference.TryGet(referenced), result);
}

void AssetDatabase::QueryRecords(const AssetRecordQuery& query, Array<AssetRecord>& result) const
{
    ScopeLock lock(_locker);
    result.Clear();

    const Array<Guid>* candidates = nullptr;
    Array<Guid> usedByCandidates;
    Array<Guid> pathCandidates;
    bool constrained = false;
    bool impossible = false;
    const auto consider = [&candidates, &constrained, &impossible](const Array<Guid>* values)
    {
        constrained = true;
        if (!values)
        {
            impossible = true;
            return;
        }
        if (!candidates || values->Count() < candidates->Count())
            candidates = values;
    };

    if (query.PathPrefix.HasChars())
    {
        const String prefix = NormalizeIndexKey(query.PathPrefix);
        int32 left = 0;
        int32 right = _recordsBySortedPath.Count();
        while (left < right)
        {
            const int32 middle = left + (right - left) / 2;
            const AssetRecord* record = _records.TryGet(_recordsBySortedPath[middle]);
            const String path = record ? NormalizeIndexKey(record->SourcePath.Get()) : String::Empty;
            if (path < prefix)
                left = middle + 1;
            else
                right = middle;
        }
        while (left < _recordsBySortedPath.Count())
        {
            const Guid& id = _recordsBySortedPath[left++];
            const AssetRecord* record = _records.TryGet(id);
            if (!record || !NormalizeIndexKey(record->SourcePath.Get()).StartsWith(prefix))
                break;
            pathCandidates.Add(id);
        }
        consider(&pathCandidates);
    }
    if (query.TypeName.HasChars())
        consider(_recordsByType.TryGet(NormalizeIndexKey(query.TypeName)));
    if (query.ProcessorId.HasChars())
        consider(_recordsByProcessor.TryGet(NormalizeIndexKey(query.ProcessorId)));
    if (query.Label.HasChars())
        consider(_recordsByLabel.TryGet(NormalizeIndexKey(query.Label)));
    if (query.HasStatus)
        consider(_recordsByStatus.TryGet(query.Status));
    if (query.ReferencedAsset.IsValid())
        consider(_referencersByRuntimeReference.TryGet(query.ReferencedAsset));
    if (query.UsedByAsset.IsValid())
    {
        const Guid* ownerId = _recordByObject.TryGet(query.UsedByAsset);
        const AssetRecord* owner = ownerId ? _records.TryGet(*ownerId) : nullptr;
        if (owner)
        {
            for (const AssetObjectId& referenced : owner->RuntimeReferences)
            {
                const Guid* id = _recordByObject.TryGet(referenced);
                if (id && !usedByCandidates.Contains(*id))
                    usedByCandidates.Add(*id);
            }
        }
        consider(owner ? &usedByCandidates : nullptr);
    }
    if (query.Name.HasChars())
    {
        const String name = NormalizeIndexKey(query.Name);
        const int32 gramLength = Math::Min(3, name.Length());
        consider(gramLength > 0 ? _recordsBySearchGram.TryGet(String(name.Get(), gramLength)) : nullptr);
    }
    if (impossible)
        return;

    const String normalizedPath = NormalizeIndexKey(query.PathPrefix);
    const String normalizedType = NormalizeIndexKey(query.TypeName);
    const String normalizedProcessor = NormalizeIndexKey(query.ProcessorId);
    const String normalizedLabel = NormalizeIndexKey(query.Label);
    const String normalizedName = NormalizeIndexKey(query.Name);
    const auto matches = [&](const AssetRecord& record)
    {
        if (query.MainAssetsOnly && !record.IsMainAsset())
            return false;
        if (query.HasStatus && record.Status != query.Status)
            return false;
        if (normalizedPath.HasChars() && !NormalizeIndexKey(record.SourcePath.Get()).StartsWith(normalizedPath))
            return false;
        if (normalizedType.HasChars())
        {
            const String recordType = NormalizeIndexKey(record.TypeName);
            String typeSuffix(TEXT("."));
            typeSuffix += normalizedType;
            if (recordType != normalizedType && !recordType.EndsWith(typeSuffix))
                return false;
        }
        if (normalizedProcessor.HasChars() && NormalizeIndexKey(record.ProcessorID) != normalizedProcessor)
            return false;
        if (normalizedLabel.HasChars())
        {
            bool found = false;
            for (const String& label : record.Labels)
                found |= NormalizeIndexKey(label) == normalizedLabel;
            if (!found)
                return false;
        }
        if (query.ReferencedAsset.IsValid() && !record.RuntimeReferences.Contains(query.ReferencedAsset))
            return false;
        if (query.UsedByAsset.IsValid() && !usedByCandidates.Contains(record.ID))
            return false;
        if (normalizedName.HasChars() && !GetSearchText(record).Contains(normalizedName))
            return false;
        return true;
    };

    if (constrained)
    {
        if (!candidates)
            return;
        result.EnsureCapacity(candidates->Count());
        for (const Guid& id : *candidates)
        {
            const AssetRecord* record = _records.TryGet(id);
            if (record && matches(*record))
                result.Add(*record);
        }
    }
    else
    {
        result.EnsureCapacity(_records.Count());
        for (const auto& entry : _records)
            if (matches(entry.Value))
                result.Add(entry.Value);
    }
    if (result.Count() > 1)
    {
        std::sort(result.Get(), result.Get() + result.Count(), [](const AssetRecord& a, const AssetRecord& b)
        {
            const int32 path = StringUtils::CompareIgnoreCase(a.SourcePath.Get().Get(), b.SourcePath.Get().Get());
            return path != 0 ? path < 0 : a.LocalId < b.LocalId;
        });
    }
}

void AssetDatabase::GetLabels(const Guid& sourceId, Array<String>& result) const
{
    result.Clear();
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    if (snapshot.IsValid())
        snapshot.GetLabels(sourceId, result);
    if (result.Count() > 1)
        std::sort(result.Get(), result.Get() + result.Count());
}

bool AssetDatabase::SetLabels(const Guid& sourceId, const Array<String>& labels, AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    SourceAssetRow source;
    if (!sourceId.IsValid() || !snapshot.IsValid() || !snapshot.TryGetSource(sourceId, source))
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SourceMissing, StringView::Empty, TEXT("Cannot label an unknown source asset."));
    Array<String> normalized;
    HashSet<String> unique;
    for (const String& label : labels)
    {
        if (label.IsEmpty())
            return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, source.Path, TEXT("Asset labels cannot be empty."));
        if (unique.Add(label))
            normalized.Add(label);
    }
    if (normalized.Count() > 1)
        std::sort(normalized.Get(), normalized.Get() + normalized.Count());
    Array<String> current;
    snapshot.GetLabels(sourceId, current);
    if (current.Count() > 1)
        std::sort(current.Get(), current.Get() + current.Count());
    if (current == normalized)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    std::unique_ptr<AssetDatabaseTransaction> transaction = _sourceDatabase.BeginTransaction();
    if (!transaction)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, source.Path, TEXT("Cannot begin the asset label transaction."));
    transaction->SetLabels(sourceId, normalized);
    if (transaction->Commit(diagnostic))
        return true;
    AssetDatabaseChangeBatch changes;
    RebuildCacheFromDurable(&changes);
    Changed(changes);
    return false;
}

bool AssetDatabase::RegisterCustomDependency(const StringView& name, const ContentHash& hash, const StringView& provider,
    AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    if (name.IsEmpty() || hash.IsZero())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::InvalidMeta, StringView::Empty, TEXT("Custom dependencies require a name and non-zero content hash."));
    SourceCustomDependencyRow current;
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    if (snapshot.IsValid() && snapshot.TryGetCustomDependency(name, current) && current.CurrentHash == hash && current.Provider == provider)
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    std::unique_ptr<AssetDatabaseTransaction> transaction = _sourceDatabase.BeginTransaction();
    if (!transaction)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, StringView::Empty, TEXT("Cannot begin the custom dependency transaction."));
    SourceCustomDependencyRow dependency;
    dependency.DependencyName = name;
    dependency.CurrentHash = hash;
    dependency.Provider = provider;
    transaction->UpsertCustomDependency(dependency);
    if (transaction->Commit(diagnostic))
        return true;
    AssetDatabaseChangeBatch changes;
    RebuildCacheFromDurable(&changes);
    Changed(changes);
    return false;
}

bool AssetDatabase::UnregisterCustomDependency(const StringView& name, AssetPipelineDiagnostic& diagnostic)
{
    ScopeLock writeLock(_writeLocker);
    SourceCustomDependencyRow current;
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    if (name.IsEmpty() || !snapshot.IsValid() || !snapshot.TryGetCustomDependency(name, current))
    {
        diagnostic = AssetPipelineDiagnostic();
        return false;
    }
    std::unique_ptr<AssetDatabaseTransaction> transaction = _sourceDatabase.BeginTransaction();
    if (!transaction)
        return Fail(diagnostic, AssetPipelineDiagnosticCode::SnapshotInvalid, StringView::Empty, TEXT("Cannot begin the custom dependency transaction."));
    transaction->RemoveCustomDependency(name);
    if (transaction->Commit(diagnostic))
        return true;
    AssetDatabaseChangeBatch changes;
    RebuildCacheFromDurable(&changes);
    Changed(changes);
    return false;
}

bool AssetDatabase::TryGetCustomDependencyHash(const StringView& name, ContentHash& result) const
{
    result = ContentHash();
    SourceCustomDependencyRow row;
    const AssetDatabaseReadSnapshot snapshot = _sourceDatabase.Read();
    if (!snapshot.IsValid() || !snapshot.TryGetCustomDependency(name, row))
        return false;
    result = row.CurrentHash;
    return true;
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
    Dictionary<String, Array<Guid>> nextRecordsByType;
    Dictionary<String, Array<Guid>> nextRecordsByLabel;
    Array<Guid> nextRecordsBySortedPath;
    Dictionary<String, Array<Guid>> nextRecordsBySearchGram;
    Dictionary<AssetRecordStatus, Array<Guid>> nextRecordsByStatus;
    Dictionary<AssetObjectId, Array<Guid>> nextDependantsByBuildInput;
    Dictionary<AssetObjectId, Array<Guid>> nextReferencersByRuntimeReference;

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
        AddToIndex(nextRecordsByProcessor, NormalizeIndexKey(entry.Value.ProcessorID), entry.Key);
        const String normalizedType = NormalizeIndexKey(entry.Value.TypeName);
        AddToIndex(nextRecordsByType, normalizedType, entry.Key);
        const int32 typeSeparator = normalizedType.FindLast('.');
        if (typeSeparator != -1 && typeSeparator + 1 < normalizedType.Length())
            AddToIndex(nextRecordsByType, normalizedType.Substring(typeSeparator + 1), entry.Key);
        for (const String& label : entry.Value.Labels)
            AddToIndex(nextRecordsByLabel, NormalizeIndexKey(label), entry.Key);
        nextRecordsBySortedPath.Add(entry.Key);
        AddSearchGrams(nextRecordsBySearchGram, entry.Value);
        AddToIndex(nextRecordsByStatus, entry.Value.Status, entry.Key);
        for (const AssetObjectId& dependency : entry.Value.BuildInputDependencies)
            AddToIndex(nextDependantsByBuildInput, dependency, entry.Key);
        for (const AssetObjectId& reference : entry.Value.RuntimeReferences)
            AddToIndex(nextReferencersByRuntimeReference, reference, entry.Key);
    }
    if (nextRecordsBySortedPath.Count() > 1)
    {
        std::sort(nextRecordsBySortedPath.Get(), nextRecordsBySortedPath.Get() + nextRecordsBySortedPath.Count(), [&nextRecords](const Guid& a, const Guid& b)
        {
            const AssetRecord* left = nextRecords.TryGet(a);
            const AssetRecord* right = nextRecords.TryGet(b);
            return left && right && NormalizeIndexKey(left->SourcePath.Get()) < NormalizeIndexKey(right->SourcePath.Get());
        });
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
        _recordsByType = MoveTemp(nextRecordsByType);
        _recordsByLabel = MoveTemp(nextRecordsByLabel);
        _recordsBySortedPath = MoveTemp(nextRecordsBySortedPath);
        _recordsBySearchGram = MoveTemp(nextRecordsBySearchGram);
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
        transaction->SetLabels(sourceId, sourceRecord->Labels);
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
        object.DisplayName = record.DisplayName.HasChars() ? record.DisplayName : String(StringUtils::GetFileNameWithoutExtension(record.SourcePath.Get()));
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
        for (const AssetObjectId& dependencyId : record.BuildInputDependencies)
        {
            SourceAssetDependencyRow dependency;
            dependency.OwnerAssetGuid = record.SourceAssetID;
            dependency.OwnerLocalFileId = record.LocalId;
            dependency.TargetId = TEXT("default");
            dependency.Kind = AssetDependencyKind::BuildInput;
            dependency.TargetAssetGuid = dependencyId.Asset.Value;
            dependency.TargetLocalFileId = dependencyId.LocalId;
            dependency.CustomDependency = dependencyId.ToString();
            dependencies->Add(MoveTemp(dependency));
        }
        for (const AssetObjectId& dependencyId : record.RuntimeReferences)
        {
            SourceAssetDependencyRow dependency;
            dependency.OwnerAssetGuid = record.SourceAssetID;
            dependency.OwnerLocalFileId = record.LocalId;
            dependency.TargetId = TEXT("default");
            dependency.Kind = AssetDependencyKind::RuntimeReference;
            dependency.TargetAssetGuid = dependencyId.Asset.Value;
            dependency.TargetLocalFileId = dependencyId.LocalId;
            dependency.CustomDependency = dependencyId.ToString();
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
