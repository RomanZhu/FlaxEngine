// Copyright (c) Wojciech Figat. All rights reserved.

#include "LoadedAssetRegistry.h"
#include "Engine/Core/Collections/HashSet.h"

struct LoadedAssetRegistry::Entry
{
    LoadedAssetRecord Record;
    uint64 Attempt = 0;
};

namespace
{
    bool Fail(AssetPipelineDiagnostic& diagnostic, AssetPipelineDiagnosticCode code, const Guid& object,
        const StringView& message)
    {
        diagnostic = AssetPipelineDiagnostic();
        diagnostic.Code = code;
        diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        diagnostic.AssetGuid = object;
        diagnostic.Message = message;
        return true;
    }
}

LoadedAssetRegistry::~LoadedAssetRegistry()
{
    _entries.ClearDelete();
}

LoadedAssetAcquireResult LoadedAssetRegistry::AcquireLoad(const Guid& object, LoadedAssetLoadTicket& ticket,
    LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    ticket = LoadedAssetLoadTicket();
    record = LoadedAssetRecord();
    if (!object.IsValid())
    {
        Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, object, TEXT("Cannot load an invalid asset object ID."));
        return LoadedAssetAcquireResult::Invalid;
    }

    _locker.Lock();
    Entry** entryPtr = _entries.TryGet(object);
    if (!entryPtr)
    {
        Entry* entry = New<Entry>();
        entry->Record.Object = object;
        _entries.Add(object, entry);
        entryPtr = _entries.TryGet(object);
    }
    Entry* entry = *entryPtr;
    bool joined = false;
    while (entry->Record.State == LoadedAssetState::Loading)
    {
        joined = true;
        _changed.Wait(_locker);
    }
    if (joined)
    {
        record = entry->Record;
        diagnostic = record.Diagnostic;
        _locker.Unlock();
        return LoadedAssetAcquireResult::Joined;
    }
    if (entry->Record.State == LoadedAssetState::Loaded ||
        entry->Record.State == LoadedAssetState::Failed || entry->Record.State == LoadedAssetState::Deleted)
    {
        record = entry->Record;
        diagnostic = record.State == LoadedAssetState::Loaded
            ? AssetPipelineDiagnostic()
            : record.Diagnostic;
        _locker.Unlock();
        return LoadedAssetAcquireResult::Ready;
    }

    entry->Attempt++;
    entry->Record.State = LoadedAssetState::Loading;
    entry->Record.Instance = nullptr;
    entry->Record.Diagnostic = AssetPipelineDiagnostic();
    ticket.Object = object;
    ticket.Attempt = entry->Attempt;
    record = entry->Record;
    diagnostic = AssetPipelineDiagnostic();
    _locker.Unlock();
    return LoadedAssetAcquireResult::Owner;
}

bool LoadedAssetRegistry::CompleteLoad(const LoadedAssetLoadTicket& ticket, void* instance, const StringAnsiView& typeName,
    const ContentHash& content, uint64 revision,
    const Array<Guid>& dependencies, const AssetPipelineDiagnostic& loadDiagnostic,
    LoadedAssetRecord& record, AssetPipelineDiagnostic& diagnostic)
{
    _locker.Lock();
    Entry** entryPtr = _entries.TryGet(ticket.Object);
    if (!entryPtr || (*entryPtr)->Attempt != ticket.Attempt || (*entryPtr)->Record.State != LoadedAssetState::Loading)
    {
        _locker.Unlock();
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, ticket.Object,
            TEXT("Asset load completion no longer owns the current registry attempt."));
    }
    Entry* entry = *entryPtr;
    if (instance && !typeName.IsEmpty() && !content.IsZero() && revision != 0)
    {
        entry->Record.State = LoadedAssetState::Loaded;
        entry->Record.Instance = instance;
        entry->Record.TypeName = typeName;
        entry->Record.Content = content;
        entry->Record.Revision = revision;
        entry->Record.Dependencies = dependencies;
        entry->Record.Diagnostic = AssetPipelineDiagnostic();
        entry->Record.StaleInstance = nullptr;
        entry->Record.StaleTypeName.Clear();
        entry->Record.StaleContent = ContentHash();
        entry->Record.StaleRevision = 0;
        entry->Record.StaleDependencies.Clear();
    }
    else
    {
        entry->Record.State = LoadedAssetState::Unresolved;
        entry->Record.Instance = nullptr;
        entry->Record.TypeName.Clear();
        entry->Record.Content = ContentHash();
        entry->Record.Revision = 0;
        entry->Record.Dependencies.Clear();
        entry->Record.Diagnostic = loadDiagnostic;
    }
    record = entry->Record;
    diagnostic = entry->Record.Diagnostic;
    _changed.NotifyAll();
    _locker.Unlock();
    return false;
}

bool LoadedAssetRegistry::TryGet(const Guid& object, LoadedAssetRecord& record) const
{
    _locker.Lock();
    Entry** entry = _entries.TryGet(object);
    if (entry)
        record = (*entry)->Record;
    _locker.Unlock();
    return entry != nullptr;
}

bool LoadedAssetRegistry::Remove(const Guid& object, void* instance)
{
    ScopeLock lock(_locker);
    Entry** entry = _entries.TryGet(object);
    if (!entry || (*entry)->Record.State == LoadedAssetState::Loading ||
        ((*entry)->Record.Instance != instance && (*entry)->Record.StaleInstance != instance))
        return true;
    Delete(*entry);
    _entries.Remove(object);
    return false;
}

bool LoadedAssetRegistry::ReplaceBatch(const Array<LoadedAssetReplacement>& replacements, Array<LoadedAssetSwap>& swaps,
    AssetPipelineDiagnostic& diagnostic)
{
    Array<Guid> removals;
    Array<LoadedAssetInvalidation> invalidations;
    return PublishBatch(replacements, removals, false, swaps, invalidations, diagnostic);
}

bool LoadedAssetRegistry::PublishBatch(const Array<LoadedAssetReplacement>& replacements, const Array<Guid>& removals,
    bool retainStale,
    Array<LoadedAssetSwap>& swaps, Array<LoadedAssetInvalidation>& invalidations,
    AssetPipelineDiagnostic& diagnostic)
{
    swaps.Clear();
    invalidations.Clear();
    if (replacements.IsEmpty() && removals.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
            TEXT("Asset publication batch cannot be empty."));

    _locker.Lock();
    HashSet<Guid> objects;
    for (const LoadedAssetReplacement& replacement : replacements)
    {
        Entry** entry = _entries.TryGet(replacement.Object);
        if (!replacement.Object.IsValid() || !replacement.Instance || replacement.TypeName.IsEmpty() ||
            replacement.Content.IsZero() || replacement.Revision == 0 ||
            !objects.Add(replacement.Object) || !entry ||
            ((*entry)->Record.State != LoadedAssetState::Loaded &&
             (*entry)->Record.State != LoadedAssetState::Failed &&
             (*entry)->Record.State != LoadedAssetState::Deleted))
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, replacement.Object,
                TEXT("Asset replacement requires one unique loaded or unavailable object."));
        }
        const uint64 previousRevision = (*entry)->Record.State == LoadedAssetState::Loaded
            ? (*entry)->Record.Revision
            : (*entry)->Record.StaleRevision;
        if (replacement.Revision <= previousRevision)
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, replacement.Object,
                TEXT("Asset replacement revision is stale."));
        }
    }
    for (const Guid& removal : removals)
    {
        Entry** entry = _entries.TryGet(removal);
        if (!removal.IsValid() || !objects.Add(removal) || !entry || (*entry)->Record.State != LoadedAssetState::Loaded)
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::ArtifactMissing, removal,
                TEXT("Asset invalidation requires one unique, currently loaded object."));
        }
    }

    swaps.EnsureCapacity(replacements.Count());
    for (const LoadedAssetReplacement& replacement : replacements)
    {
        Entry* entry = *_entries.TryGet(replacement.Object);
        LoadedAssetSwap swap;
        swap.Object = replacement.Object;
        const bool wasUnavailable = entry->Record.State == LoadedAssetState::Failed ||
            entry->Record.State == LoadedAssetState::Deleted;
        swap.PreviousInstance = wasUnavailable ? entry->Record.StaleInstance : entry->Record.Instance;
        swap.PreviousTypeName = wasUnavailable ? entry->Record.StaleTypeName : entry->Record.TypeName;
        swap.PreviousContent = wasUnavailable ? entry->Record.StaleContent : entry->Record.Content;
        swap.PreviousRevision = wasUnavailable ? entry->Record.StaleRevision : entry->Record.Revision;
        swap.Instance = replacement.Instance;
        swap.TypeName = replacement.TypeName;
        swap.Content = replacement.Content;
        swap.Revision = replacement.Revision;
        swaps.Add(swap);
        entry->Record.Instance = replacement.Instance;
        entry->Record.TypeName = replacement.TypeName;
        entry->Record.Content = replacement.Content;
        entry->Record.Revision = replacement.Revision;
        entry->Record.Dependencies = replacement.Dependencies;
        entry->Record.Diagnostic = AssetPipelineDiagnostic();
        entry->Record.State = LoadedAssetState::Loaded;
        entry->Record.StaleInstance = nullptr;
        entry->Record.StaleTypeName.Clear();
        entry->Record.StaleContent = ContentHash();
        entry->Record.StaleRevision = 0;
        entry->Record.StaleDependencies.Clear();
    }
    invalidations.EnsureCapacity(removals.Count());
    for (const Guid& removal : removals)
    {
        Entry* entry = *_entries.TryGet(removal);
        LoadedAssetInvalidation invalidation;
        invalidation.Object = removal;
        invalidation.PreviousInstance = entry->Record.Instance;
        invalidation.PreviousTypeName = entry->Record.TypeName;
        invalidation.PreviousContent = entry->Record.Content;
        invalidation.PreviousRevision = entry->Record.Revision;
        invalidation.State = LoadedAssetState::Deleted;
        invalidation.Diagnostic.Code = AssetPipelineDiagnosticCode::SourceMissing;
        invalidation.Diagnostic.Stage = AssetPipelineDiagnosticStage::Resolution;
        invalidation.Diagnostic.AssetGuid = removal;
        invalidation.Diagnostic.Message = TEXT("Asset object was removed from the published source inventory.");
        entry->Record.Diagnostic = invalidation.Diagnostic;
        invalidations.Add(MoveTemp(invalidation));
        entry->Record.StaleInstance = retainStale ? entry->Record.Instance : nullptr;
        entry->Record.StaleTypeName = retainStale ? entry->Record.TypeName : StringAnsi();
        entry->Record.StaleContent = retainStale ? entry->Record.Content : ContentHash();
        entry->Record.StaleRevision = retainStale ? entry->Record.Revision : 0;
        entry->Record.StaleDependencies = retainStale ? entry->Record.Dependencies : Array<Guid>();
        entry->Record.State = LoadedAssetState::Deleted;
        entry->Record.Instance = nullptr;
        entry->Record.TypeName.Clear();
        entry->Record.Content = ContentHash();
        entry->Record.Revision = 0;
        entry->Record.Dependencies.Clear();
    }
    diagnostic = AssetPipelineDiagnostic();
    _locker.Unlock();
    return false;
}

bool LoadedAssetRegistry::TransitionBatch(const Array<LoadedAssetTransition>& transitions, bool retainStale,
    Array<LoadedAssetInvalidation>& invalidations, AssetPipelineDiagnostic& diagnostic)
{
    invalidations.Clear();
    if (transitions.IsEmpty())
        return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, Guid::Empty,
            TEXT("Asset state transition batch cannot be empty."));

    _locker.Lock();
    HashSet<Guid> objects;
    for (const LoadedAssetTransition& transition : transitions)
    {
        Entry** entry = _entries.TryGet(transition.Object);
        if (!transition.Object.IsValid() ||
            (transition.State != LoadedAssetState::Failed && transition.State != LoadedAssetState::Deleted) ||
            transition.Diagnostic.Code == AssetPipelineDiagnosticCode::None || !objects.Add(transition.Object) ||
            !entry || ((*entry)->Record.State != LoadedAssetState::Loaded &&
                       (*entry)->Record.State != LoadedAssetState::Unresolved))
        {
            _locker.Unlock();
            return Fail(diagnostic, AssetPipelineDiagnosticCode::PrepareInvalidated, transition.Object,
                TEXT("Asset state transition requires one unique loaded or unresolved object and a failure diagnostic."));
        }
    }

    invalidations.EnsureCapacity(transitions.Count());
    for (const LoadedAssetTransition& transition : transitions)
    {
        Entry* entry = *_entries.TryGet(transition.Object);
        LoadedAssetInvalidation invalidation;
        invalidation.Object = transition.Object;
        invalidation.PreviousInstance = entry->Record.Instance;
        invalidation.PreviousTypeName = entry->Record.TypeName;
        invalidation.PreviousContent = entry->Record.Content;
        invalidation.PreviousRevision = entry->Record.Revision;
        invalidation.State = transition.State;
        invalidation.Diagnostic = transition.Diagnostic;
        invalidations.Add(MoveTemp(invalidation));

        entry->Record.StaleInstance = retainStale ? entry->Record.Instance : nullptr;
        entry->Record.StaleTypeName = retainStale ? entry->Record.TypeName : StringAnsi();
        entry->Record.StaleContent = retainStale ? entry->Record.Content : ContentHash();
        entry->Record.StaleRevision = retainStale ? entry->Record.Revision : 0;
        entry->Record.StaleDependencies = retainStale ? entry->Record.Dependencies : Array<Guid>();
        entry->Record.State = transition.State;
        entry->Record.Instance = nullptr;
        entry->Record.TypeName.Clear();
        entry->Record.Content = ContentHash();
        entry->Record.Revision = 0;
        entry->Record.Dependencies.Clear();
        entry->Record.Diagnostic = transition.Diagnostic;
    }
    diagnostic = AssetPipelineDiagnostic();
    _changed.NotifyAll();
    _locker.Unlock();
    return false;
}

void LoadedAssetRegistry::GetLoadedRecords(Array<LoadedAssetRecord>& records) const
{
    records.Clear();
    ScopeLock lock(_locker);
    records.EnsureCapacity(_entries.Count());
    for (const auto& entry : _entries)
    {
        if (entry.Value->Record.State == LoadedAssetState::Loaded)
            records.Add(entry.Value->Record);
    }
}

int32 LoadedAssetRegistry::Count() const
{
    _locker.Lock();
    const int32 result = _entries.Count();
    _locker.Unlock();
    return result;
}
